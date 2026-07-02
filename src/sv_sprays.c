/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
*/

#ifndef CLIENTONLY
#include "qwsvdef.h"

#ifdef MVD_PEXT1_SPRAYS

#define SV_MAX_SPRAYS 64
#define SV_MAX_SPRAY_IMAGES 64
#define SV_SPRAY_SEND_QUEUE 256
#define SV_SPRAY_NEW_IMAGE_INTERVAL 10.0
#define SV_SPRAY_HASH_OFFSET 14695981039346656037ULL
#define SV_SPRAY_HASH_PRIME 1099511628211ULL

extern cvar_t sv_spray_chunks_per_frame;

typedef struct {
	qbool active;
	unsigned short id;

	// Hash indexes the image cache. Each placed spray still keeps a pixel copy
	// so active sprays survive image-cache eviction.
	unsigned long long hash;
	int owner;
	vec3_t origin;
	vec3_t right;
	vec3_t up;
	float half_width;
	float half_height;

	// Alpha is placement metadata. It affects rendering on clients, but does
	// not affect the cached image bytes or hash.
	float alpha;

	// Dimensions describe the normalized client payload. Source files may have
	// been larger, but MVDSV only ever sees protocol-sized RGBA bytes.
	int width;
	int height;
	int byte_count;
	byte pixels[spraynet_max_bytes];
} sv_spray_t;

typedef struct {
	qbool active;
	qbool accepted;
	unsigned short local_id;
	unsigned short server_id;

	// Claimed by the client in spraynet_begin and verified once all pixels are
	// received. If the server already has this hash, no pixels are requested.
	unsigned long long hash;
	int received;
	vec3_t origin;
	vec3_t right;
	vec3_t up;
	float half_width;
	float half_height;

	// Accepted placement metadata is staged here while optional pixel chunks
	// arrive. If the image hash is already cached, no chunks are needed.
	float alpha;
	int width;
	int height;
	int byte_count;
	byte pixels[spraynet_max_bytes];
} sv_spray_upload_t;

typedef struct {
	qbool active;

	// Global image cache keyed by raw RGBA hash. This is bandwidth cache only;
	// visible spray lifetime is tracked by sv_sprays[].
	unsigned long long hash;

	// The hash includes these dimensions, but storing them with the cached image
	// lets a later placement reuse the correct payload shape without trusting
	// the requesting client's repeated metadata.
	int width;
	int height;
	int byte_count;
	byte pixels[spraynet_max_bytes];
} sv_spray_image_t;

typedef struct {
	unsigned short ids[SV_SPRAY_SEND_QUEUE];
	int head;
	int count;

	// current_id/offset let each client trickle one spray over many frames
	// without filling reliable back buffers or causing ping spikes.
	unsigned short current_id;
	int offset;
} sv_spray_sendqueue_t;

static sv_spray_t sv_sprays[SV_MAX_SPRAYS];
static sv_spray_image_t sv_spray_images[SV_MAX_SPRAY_IMAGES];
static sv_spray_upload_t sv_spray_uploads[MAX_CLIENTS];
static sv_spray_sendqueue_t sv_spray_sendqueues[MAX_CLIENTS];
static unsigned long long sv_spray_client_hashes[MAX_CLIENTS][SV_MAX_SPRAY_IMAGES];
static int sv_spray_client_hash_counts[MAX_CLIENTS];
static double sv_spray_last_new_image_time[MAX_CLIENTS];
static unsigned short sv_spray_next_id = 1;

static unsigned long long SV_SprayHashBytes(const byte *pixels, int width, int height, int byte_count)
{
	unsigned long long hash = SV_SPRAY_HASH_OFFSET;
	int i;
	unsigned int dimensions[2];

	// FNV-1a is deliberately cheap: the hash is only a cache key for normalized
	// RGBA bytes plus dimensions, not a trust or anti-cheat boundary.
	dimensions[0] = (unsigned int)width;
	dimensions[1] = (unsigned int)height;
	for (i = 0; i < (int)sizeof(dimensions); ++i) {
		hash ^= ((byte *)dimensions)[i];
		hash *= SV_SPRAY_HASH_PRIME;
	}
	for (i = 0; i < byte_count; ++i) {
		hash ^= pixels[i];
		hash *= SV_SPRAY_HASH_PRIME;
	}
	return hash;
}

static qbool SV_SprayValidImageSize(int width, int height, int byte_count)
{
	// Clients may load larger source images, but they must downsample before
	// upload. The wire payload is always RGBA within these limits.
	return width > 0 && height > 0 && width <= spraynet_max_width && height <= spraynet_max_height && byte_count == width * height * spraynet_bpp;
}

static qbool SV_SprayValidAlpha(float alpha)
{
	// Alpha is a normalized placement value. Invalid values are rejected rather
	// than clamped so malformed clients cannot create ambiguous state.
	return alpha >= 0 && alpha <= 1;
}

static qbool SV_SprayNewImageAllowed(client_t *client)
{
	int client_index = client - svs.clients;
	double elapsed = curtime - sv_spray_last_new_image_time[client_index];

	// rate limit new image uploads to once every 10 seconds per client
	if (sv_spray_last_new_image_time[client_index] > 0 && elapsed >= 0 && elapsed < SV_SPRAY_NEW_IMAGE_INTERVAL) {
		int remaining = (int)(SV_SPRAY_NEW_IMAGE_INTERVAL - elapsed + 0.999);

		SV_ClientPrintf(client, PRINT_HIGH, "spray: wait %d seconds before uploading a new spray image\n", max(1, remaining));
		return false;
	}

	sv_spray_last_new_image_time[client_index] = curtime;
	return true;
}

static void SV_SprayWriteHash(client_t *client, unsigned long long hash)
{
	// Quake messages have 32-bit primitives, so split the 64-bit hash little
	// word first. The client reassembles it the same way.
	ClientReliableWrite_Long(client, (int)(hash & 0xffffffffULL));
	ClientReliableWrite_Long(client, (int)(hash >> 32));
}

static unsigned long long SV_SprayReadHash(void)
{
	unsigned int lo = (unsigned int)MSG_ReadLong();
	unsigned int hi = (unsigned int)MSG_ReadLong();

	return ((unsigned long long)hi << 32) | lo;
}

static qbool SV_SprayClientSupports(client_t *client)
{
	return client->state >= cs_connected && (client->mvdprotocolextensions1 & MVD_PEXT1_SPRAYS);
}

static int SV_SprayClientIndex(client_t *client)
{
	return client - svs.clients;
}

static unsigned short SV_SprayAllocId(void)
{
	unsigned short id = sv_spray_next_id++;

	if (!sv_spray_next_id) {
		sv_spray_next_id = 1;
	}
	return id;
}

static sv_spray_t *SV_SprayFind(int id)
{
	int i;

	for (i = 0; i < SV_MAX_SPRAYS; ++i) {
		if (sv_sprays[i].active && sv_sprays[i].id == id) {
			return &sv_sprays[i];
		}
	}
	return NULL;
}

static sv_spray_image_t *SV_SprayImageFind(unsigned long long hash)
{
	int i;

	for (i = 0; i < SV_MAX_SPRAY_IMAGES; ++i) {
		if (sv_spray_images[i].active && sv_spray_images[i].hash == hash) {
			return &sv_spray_images[i];
		}
	}
	return NULL;
}

static sv_spray_image_t *SV_SprayImageAlloc(unsigned long long hash)
{
	int i;

	for (i = 0; i < SV_MAX_SPRAY_IMAGES; ++i) {
		if (!sv_spray_images[i].active) {
			memset(&sv_spray_images[i], 0, sizeof(sv_spray_images[i]));
			sv_spray_images[i].active = true;
			sv_spray_images[i].hash = hash;
			return &sv_spray_images[i];
		}
	}

	memset(&sv_spray_images[0], 0, sizeof(sv_spray_images[0]));
	sv_spray_images[0].active = true;
	sv_spray_images[0].hash = hash;
	return &sv_spray_images[0];
}

static qbool SV_SprayClientKnowsHash(client_t *client, unsigned long long hash)
{
	int client_index = SV_SprayClientIndex(client);
	int i;

	for (i = 0; i < sv_spray_client_hash_counts[client_index]; ++i) {
		if (sv_spray_client_hashes[client_index][i] == hash) {
			return true;
		}
	}
	return false;
}

static void SV_SprayClientRememberHash(client_t *client, unsigned long long hash)
{
	int client_index = SV_SprayClientIndex(client);
	int count = sv_spray_client_hash_counts[client_index];

	// This table means "the client can render this hash without pixels." It is
	// updated after the server sends all chunks, or when the client originated
	// the image and therefore has the local texture.
	if (SV_SprayClientKnowsHash(client, hash)) {
		return;
	}
	if (count == SV_MAX_SPRAY_IMAGES) {
		memmove(sv_spray_client_hashes[client_index], sv_spray_client_hashes[client_index] + 1, (SV_MAX_SPRAY_IMAGES - 1) * sizeof(sv_spray_client_hashes[client_index][0]));
		count = SV_MAX_SPRAY_IMAGES - 1;
	}
	sv_spray_client_hashes[client_index][count] = hash;
	sv_spray_client_hash_counts[client_index] = count + 1;
}

static sv_spray_t *SV_SprayAllocSlot(void)
{
	int i;

	for (i = 0; i < SV_MAX_SPRAYS; ++i) {
		if (!sv_sprays[i].active) {
			return &sv_sprays[i];
		}
	}

	SV_SpraysClearOne(sv_sprays[0].id, true);
	return &sv_sprays[0];
}

static void SV_SprayWriteBegin(client_t *client, const sv_spray_t *spray)
{
	ClientReliableWrite_Begin(client, svc_spray, 1 + 1 + 2 + spraynet_hash_bytes + 2 + 2 + 4 + 12 * 4);
	ClientReliableWrite_Byte(client, spraynet_begin);
	ClientReliableWrite_Short(client, spray->id);
	SV_SprayWriteHash(client, spray->hash);

	// Width/height/byte_count describe the cached image payload. The remaining
	// floats are placement metadata and may differ for every use of that image.
	ClientReliableWrite_Short(client, spray->width);
	ClientReliableWrite_Short(client, spray->height);
	ClientReliableWrite_Long(client, spray->byte_count);
	ClientReliableWrite_Float(client, spray->origin[0]);
	ClientReliableWrite_Float(client, spray->origin[1]);
	ClientReliableWrite_Float(client, spray->origin[2]);
	ClientReliableWrite_Float(client, spray->right[0]);
	ClientReliableWrite_Float(client, spray->right[1]);
	ClientReliableWrite_Float(client, spray->right[2]);
	ClientReliableWrite_Float(client, spray->up[0]);
	ClientReliableWrite_Float(client, spray->up[1]);
	ClientReliableWrite_Float(client, spray->up[2]);
	ClientReliableWrite_Float(client, spray->half_width);
	ClientReliableWrite_Float(client, spray->half_height);
	ClientReliableWrite_Float(client, spray->alpha);
}

static qbool SV_SprayReliableCanWrite(client_t *client, int maxsize)
{
	return !client->num_backbuf && client->netchan.message.cursize <= client->netchan.message.maxsize - maxsize - 1;
}

static void SV_SprayWritePixelChunk(client_t *client, int id, int offset, int chunk, const byte *pixels)
{
	ClientReliableWrite_Begin(client, svc_spray, 1 + 1 + 2 + 4 + 2 + chunk);
	ClientReliableWrite_Byte(client, spraynet_pixels);
	ClientReliableWrite_Short(client, id);
	ClientReliableWrite_Long(client, offset);
	ClientReliableWrite_Short(client, chunk);
	ClientReliableWrite_SZ(client, (void *)(pixels + offset), chunk);
}

static qbool SV_SprayQueueHasId(const sv_spray_sendqueue_t *queue, int id)
{
	int i;

	if (queue->current_id == id) {
		return true;
	}

	for (i = 0; i < queue->count; ++i) {
		int index = (queue->head + i) % SV_SPRAY_SEND_QUEUE;

		if (queue->ids[index] == id) {
			return true;
		}
	}

	return false;
}

static void SV_SprayQueueClearForId(int id)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; ++i) {
		sv_spray_sendqueue_t *queue = &sv_spray_sendqueues[i];
		unsigned short kept[SV_SPRAY_SEND_QUEUE];
		int j, kept_count = 0;

		if (queue->current_id == id) {
			queue->current_id = 0;
			queue->offset = 0;
		}

		for (j = 0; j < queue->count; ++j) {
			int index = (queue->head + j) % SV_SPRAY_SEND_QUEUE;

			if (queue->ids[index] != id) {
				kept[kept_count++] = queue->ids[index];
			}
		}

		memcpy(queue->ids, kept, kept_count * sizeof(kept[0]));
		queue->head = 0;
		queue->count = kept_count;
	}
}

static void SV_SprayQueueForClient(client_t *client, int id)
{
	sv_spray_sendqueue_t *queue;
	int tail;

	if (!SV_SprayClientSupports(client) || !SV_SprayFind(id)) {
		return;
	}

	queue = &sv_spray_sendqueues[SV_SprayClientIndex(client)];
	if (SV_SprayQueueHasId(queue, id)) {
		return;
	}

	if (queue->count == SV_SPRAY_SEND_QUEUE) {
		// Drop the oldest pending placement instead of growing memory or
		// flooding a reconnecting client with reliable spray data.
		queue->head = (queue->head + 1) % SV_SPRAY_SEND_QUEUE;
		--queue->count;
	}

	tail = (queue->head + queue->count) % SV_SPRAY_SEND_QUEUE;
	queue->ids[tail] = id;
	++queue->count;
}

static void SV_SprayBroadcast(const sv_spray_t *spray)
{
	int i;
	client_t *client;

	for (i = 0, client = svs.clients; i < MAX_CLIENTS; ++i, ++client) {
		if (client->state != cs_spawned || i == spray->owner) {
			continue;
		}
		SV_SprayQueueForClient(client, spray->id);
	}
}

static void SV_SprayRecordHiddenBlock(const byte *payload, int payload_len)
{
	mvdhidden_block_header_t header;

	if (!sv.mvdrecording) {
		return;
	}

	header.length = payload_len;
	header.type_id = mvdhidden_spray;
	if (MVDWrite_HiddenBlockBegin(sizeof_mvdhidden_block_header_t_range0 + payload_len)) {
		header.length = LittleLong(header.length);
		MVD_SZ_Write(&header.length, sizeof(header.length));
		MVD_SZ_Write(&header.type_id, sizeof(header.type_id));
		MVD_SZ_Write(payload, payload_len);
	}
}

static void SV_SprayRecordBegin(const sv_spray_t *spray)
{
	byte buffer[1 + 2 + spraynet_hash_bytes + 2 + 2 + 4 + 12 * 4];
	sizebuf_t msg;

	// MVD hidden spray blocks reuse the same payload shape as svc_spray, minus
	// the svc byte. Demos remain self-contained because pixels are recorded too.
	memset(&msg, 0, sizeof(msg));
	msg.data = buffer;
	msg.maxsize = sizeof(buffer);
	MSG_WriteByte(&msg, spraynet_begin);
	MSG_WriteShort(&msg, spray->id);
	MSG_WriteLong(&msg, (int)(spray->hash & 0xffffffffULL));
	MSG_WriteLong(&msg, (int)(spray->hash >> 32));

	// Record the exact same begin payload clients receive, so MVD playback can
	// reconstruct placement alpha and variable image dimensions.
	MSG_WriteShort(&msg, spray->width);
	MSG_WriteShort(&msg, spray->height);
	MSG_WriteLong(&msg, spray->byte_count);
	MSG_WriteFloat(&msg, spray->origin[0]);
	MSG_WriteFloat(&msg, spray->origin[1]);
	MSG_WriteFloat(&msg, spray->origin[2]);
	MSG_WriteFloat(&msg, spray->right[0]);
	MSG_WriteFloat(&msg, spray->right[1]);
	MSG_WriteFloat(&msg, spray->right[2]);
	MSG_WriteFloat(&msg, spray->up[0]);
	MSG_WriteFloat(&msg, spray->up[1]);
	MSG_WriteFloat(&msg, spray->up[2]);
	MSG_WriteFloat(&msg, spray->half_width);
	MSG_WriteFloat(&msg, spray->half_height);
	MSG_WriteFloat(&msg, spray->alpha);
	SV_SprayRecordHiddenBlock(msg.data, msg.cursize);
}

static void SV_SprayRecordPixels(int id, const byte *pixels, int byte_count)
{
	int offset = 0;

	while (offset < byte_count) {
		byte buffer[1 + 2 + 4 + 2 + spraynet_chunk_bytes];
		sizebuf_t msg;
		int chunk = min(spraynet_chunk_bytes, byte_count - offset);

		memset(&msg, 0, sizeof(msg));
		msg.data = buffer;
		msg.maxsize = sizeof(buffer);
		MSG_WriteByte(&msg, spraynet_pixels);
		MSG_WriteShort(&msg, id);
		MSG_WriteLong(&msg, offset);
		MSG_WriteShort(&msg, chunk);
		SZ_Write(&msg, pixels + offset, chunk);
		SV_SprayRecordHiddenBlock(msg.data, msg.cursize);
		offset += chunk;
	}
}

static void SV_SprayRecordClearOne(int id)
{
	byte buffer[1 + 2];
	sizebuf_t msg;

	memset(&msg, 0, sizeof(msg));
	msg.data = buffer;
	msg.maxsize = sizeof(buffer);
	MSG_WriteByte(&msg, spraynet_clear_one);
	MSG_WriteShort(&msg, id);
	SV_SprayRecordHiddenBlock(msg.data, msg.cursize);
}

static void SV_SprayRecordClearAll(void)
{
	byte sub = spraynet_clear_all;

	SV_SprayRecordHiddenBlock(&sub, sizeof(sub));
}

static void SV_SprayNotifyClearOne(int id)
{
	int i;
	client_t *client;

	for (i = 0, client = svs.clients; i < MAX_CLIENTS; ++i, ++client) {
		if (client->state != cs_spawned || !SV_SprayClientSupports(client)) {
			continue;
		}
		ClientReliableWrite_Begin(client, svc_spray, 1 + 1 + 2);
		ClientReliableWrite_Byte(client, spraynet_clear_one);
		ClientReliableWrite_Short(client, id);
	}
}

static void SV_SprayNotifyClearAll(void)
{
	int i;
	client_t *client;

	for (i = 0, client = svs.clients; i < MAX_CLIENTS; ++i, ++client) {
		if (client->state != cs_spawned || !SV_SprayClientSupports(client)) {
			continue;
		}
		ClientReliableWrite_Begin(client, svc_spray, 1 + 1);
		ClientReliableWrite_Byte(client, spraynet_clear_all);
	}
}

static void SV_SpraySendAcceptFlags(client_t *client, int local_id, int server_id, int flags)
{
	if (!SV_SprayClientSupports(client)) {
		return;
	}

	ClientReliableWrite_Begin(client, svc_spray, 1 + 1 + 2 + 2 + 1);
	ClientReliableWrite_Byte(client, spraynet_accept);
	ClientReliableWrite_Short(client, local_id);
	ClientReliableWrite_Short(client, server_id);
	ClientReliableWrite_Byte(client, flags);
}

static void SV_SpraySendAccept(client_t *client, int local_id, int server_id)
{
	SV_SpraySendAcceptFlags(client, local_id, server_id, spraynet_accept_need_pixels);
}

static void SV_SpraySendDeny(client_t *client, int local_id)
{
	if (!SV_SprayClientSupports(client)) {
		return;
	}

	ClientReliableWrite_Begin(client, svc_spray, 1 + 1 + 2 + 2);
	ClientReliableWrite_Byte(client, spraynet_deny);
	ClientReliableWrite_Short(client, local_id);
	ClientReliableWrite_Short(client, 0);
}

static qbool SV_SprayModAllows(client_t *client, const vec3_t origin, const vec3_t right, const vec3_t up, float half_width, float half_height)
{
	float max_half_size = max(half_width, half_height);

	// Mods own spray policy. Without the hook, advertise protocol support but
	// deny every attempt instead of allowing sprays at arbitrary game states.
	if (!GE_CanSpray) {
#ifdef USE_PR2
		return PR2_CanSpray(client->edict);
#endif
		return false;
	}

	pr_global_struct->time = sv.time;
	pr_global_struct->self = EDICT_TO_PROG(client->edict);
	VectorCopy(origin, G_VECTOR(OFS_PARM0));
	VectorCopy(right, G_VECTOR(OFS_PARM1));
	VectorCopy(up, G_VECTOR(OFS_PARM2));
	G_FLOAT(OFS_PARM3) = max_half_size;
	PR_ExecuteProgram(GE_CanSpray);
	return G_FLOAT(OFS_RETURN) != 0;
}

static void SV_SprayModPlaced(client_t *client, const sv_spray_t *spray)
{
	if (!GE_SprayPlaced) {
#ifdef USE_PR2
		PR2_SprayPlaced(client->edict, spray->id);
		return;
#endif
		return;
	}

	pr_global_struct->time = sv.time;
	pr_global_struct->self = EDICT_TO_PROG(client->edict);
	G_FLOAT(OFS_PARM0) = spray->id;
	VectorCopy(spray->origin, G_VECTOR(OFS_PARM1));
	VectorCopy(spray->right, G_VECTOR(OFS_PARM2));
	VectorCopy(spray->up, G_VECTOR(OFS_PARM3));
	G_FLOAT(OFS_PARM4) = max(spray->half_width, spray->half_height);
	PR_ExecuteProgram(GE_SprayPlaced);
}

void SV_SpraysNewMap(void)
{
	// New map is the hard cache boundary. Clear visible sprays, pending
	// transfers, global image bytes, and per-client known-hash state together.
	memset(sv_sprays, 0, sizeof(sv_sprays));
	memset(sv_spray_images, 0, sizeof(sv_spray_images));
	memset(sv_spray_uploads, 0, sizeof(sv_spray_uploads));
	memset(sv_spray_sendqueues, 0, sizeof(sv_spray_sendqueues));
	memset(sv_spray_client_hashes, 0, sizeof(sv_spray_client_hashes));
	memset(sv_spray_client_hash_counts, 0, sizeof(sv_spray_client_hash_counts));
	memset(sv_spray_last_new_image_time, 0, sizeof(sv_spray_last_new_image_time));
	sv_spray_next_id = 1;
}

void SV_SpraysSendExisting(client_t *client)
{
	int i;

	if (!SV_SprayClientSupports(client)) {
		return;
	}

	// A newly spawned client has not received any server-provided spray images
	// yet, so every existing spray starts as a full begin+pixels transfer.
	memset(&sv_spray_sendqueues[SV_SprayClientIndex(client)], 0, sizeof(sv_spray_sendqueues[0]));
	sv_spray_client_hash_counts[SV_SprayClientIndex(client)] = 0;
	for (i = 0; i < SV_MAX_SPRAYS; ++i) {
		if (sv_sprays[i].active) {
			SV_SprayQueueForClient(client, sv_sprays[i].id);
		}
	}
}

void SV_SpraysSendPending(client_t *client)
{
	sv_spray_sendqueue_t *queue;
	sv_spray_t *spray;
	int budget;

	if (!SV_SprayClientSupports(client) || client->state != cs_spawned) {
		return;
	}

	queue = &sv_spray_sendqueues[SV_SprayClientIndex(client)];
	budget = (int)sv_spray_chunks_per_frame.value;
	if (budget <= 0) {
		return;
	}

	while (budget > 0) {
		if (!queue->current_id) {
			if (!queue->count) {
				return;
			}

			queue->current_id = queue->ids[queue->head];
			queue->head = (queue->head + 1) % SV_SPRAY_SEND_QUEUE;
			--queue->count;
			queue->offset = -1;
		}

		spray = SV_SprayFind(queue->current_id);
		if (!spray) {
			queue->current_id = 0;
			queue->offset = 0;
			continue;
		}

		if (queue->offset < 0) {
			if (!SV_SprayReliableCanWrite(client, 1 + 1 + 2 + spraynet_hash_bytes + 2 + 2 + 4 + 12 * 4)) {
				return;
			}
			SV_SprayWriteBegin(client, spray);
			if (SV_SprayClientKnowsHash(client, spray->hash)) {
				// Placement-only path: client already has texture bytes for
				// this hash, so the begin message is enough.
				queue->current_id = 0;
				queue->offset = 0;
				continue;
			}
			queue->offset = 0;
			// Keep begin and pixel chunks in separate send passes. Reliable
			// ordering should preserve them either way, but join-time signon
			// traffic is busy; this guarantees the client creates the receive
			// buffer before any spray pixels are queued behind it.
			return;
		}

		if (queue->offset < spray->byte_count) {
			int chunk = min(spraynet_chunk_bytes, spray->byte_count - queue->offset);

			if (!SV_SprayReliableCanWrite(client, 1 + 1 + 2 + 4 + 2 + chunk)) {
				return;
			}
			SV_SprayWritePixelChunk(client, spray->id, queue->offset, chunk, spray->pixels);
			queue->offset += chunk;
			--budget;
			if (queue->offset == spray->byte_count) {
				SV_SprayClientRememberHash(client, spray->hash);
				queue->current_id = 0;
				queue->offset = 0;
			}
		}
	}
}

void SV_SpraysClearAll(qbool notify)
{
	// Clear visible state and in-flight transfers, but keep image/hash caches.
	// This lets later sprays reuse images without another RGBA upload.
	memset(sv_sprays, 0, sizeof(sv_sprays));
	memset(sv_spray_uploads, 0, sizeof(sv_spray_uploads));
	memset(sv_spray_sendqueues, 0, sizeof(sv_spray_sendqueues));

	if (notify) {
		SV_SprayNotifyClearAll();
		SV_SprayRecordClearAll();
	}
}

qbool SV_SpraysClearOne(int id, qbool notify)
{
	sv_spray_t *spray = SV_SprayFind(id);

	if (!spray) {
		return false;
	}

	memset(spray, 0, sizeof(*spray));
	SV_SprayQueueClearForId(id);
	if (notify) {
		SV_SprayNotifyClearOne(id);
		SV_SprayRecordClearOne(id);
	}
	return true;
}

static sv_spray_t *SV_SprayStore(client_t *client, const sv_spray_upload_t *upload, const byte *pixels)
{
	sv_spray_t *spray = SV_SprayAllocSlot();

	memset(spray, 0, sizeof(*spray));
	spray->active = true;
	spray->id = upload->server_id;
	spray->hash = upload->hash;
	spray->owner = client - svs.clients;
	VectorCopy(upload->origin, spray->origin);
	VectorCopy(upload->right, spray->right);
	VectorCopy(upload->up, spray->up);
	spray->half_width = upload->half_width;
	spray->half_height = upload->half_height;
	spray->alpha = upload->alpha;
	spray->width = upload->width;
	spray->height = upload->height;
	spray->byte_count = upload->byte_count;
	memcpy(spray->pixels, pixels, spray->byte_count);

	return spray;
}

static void SV_SprayFinalize(client_t *client, const sv_spray_upload_t *upload, const byte *pixels)
{
	sv_spray_t *spray = SV_SprayStore(client, upload, pixels);

	// Store first so broadcast, demo recording, and the mod callback all refer
	// to the same authoritative server id and placement.
	SV_SprayBroadcast(spray);
	SV_SprayRecordBegin(spray);
	SV_SprayRecordPixels(spray->id, spray->pixels, spray->byte_count);
	SV_SprayModPlaced(client, spray);
}

static void SV_SpraysParseBegin(client_t *client)
{
	int local_id = MSG_ReadShort();
	unsigned long long hash = SV_SprayReadHash();
	int width = MSG_ReadShort();
	int height = MSG_ReadShort();
	int byte_count = MSG_ReadLong();
	vec3_t origin, right, up;
	float half_width, half_height;
	float alpha;
	sv_spray_upload_t *upload = &sv_spray_uploads[client - svs.clients];
	sv_spray_image_t *cached;

	origin[0] = MSG_ReadFloat();
	origin[1] = MSG_ReadFloat();
	origin[2] = MSG_ReadFloat();
	right[0] = MSG_ReadFloat();
	right[1] = MSG_ReadFloat();
	right[2] = MSG_ReadFloat();
	up[0] = MSG_ReadFloat();
	up[1] = MSG_ReadFloat();
	up[2] = MSG_ReadFloat();
	half_width = MSG_ReadFloat();
	half_height = MSG_ReadFloat();
	alpha = MSG_ReadFloat();

	if (!SV_SprayClientSupports(client) || !SV_SprayValidImageSize(width, height, byte_count) || half_width <= 0 || half_width > 4096 || half_height <= 0 || half_height > 4096 || !SV_SprayValidAlpha(alpha)) {
		SV_SpraySendDeny(client, local_id);
		memset(upload, 0, sizeof(*upload));
		return;
	}

	// Mods decide policy from placement data and player identity only. The image
	// bytes are not uploaded until after the mod has allowed this attempt.
	if (!SV_SprayModAllows(client, origin, right, up, half_width, half_height)) {
		SV_SpraySendDeny(client, local_id);
		memset(upload, 0, sizeof(*upload));
		return;
	}

	memset(upload, 0, sizeof(*upload));
	upload->active = true;
	upload->accepted = true;
	upload->local_id = local_id;
	upload->server_id = SV_SprayAllocId();
	upload->hash = hash;
	upload->width = width;
	upload->height = height;
	upload->byte_count = byte_count;
	VectorCopy(origin, upload->origin);
	VectorCopy(right, upload->right);
	VectorCopy(up, upload->up);
	upload->half_width = half_width;
	upload->half_height = half_height;
	upload->alpha = alpha;

	cached = SV_SprayImageFind(hash);
	if (cached) {
		// A hash hit means MVDSV already has canonical dimensions and pixels.
		// Prefer the cached metadata instead of trusting the repeated client
		// dimensions that accompanied the hash-only placement request.
		upload->width = cached->width;
		upload->height = cached->height;
		upload->byte_count = cached->byte_count;
		// The server already has the image bytes. Accept with no-pixels-needed;
		// the sender can render placement-only from its own local image cache.
		SV_SpraySendAcceptFlags(client, local_id, upload->server_id, 0);
		SV_SprayClientRememberHash(client, hash);
		SV_SprayFinalize(client, upload, cached->pixels);
		memset(upload, 0, sizeof(*upload));
		return;
	}

	if (!SV_SprayNewImageAllowed(client)) {
		SV_SpraySendDeny(client, local_id);
		memset(upload, 0, sizeof(*upload));
		return;
	}

	SV_SpraySendAccept(client, local_id, upload->server_id);
}

static void SV_SpraysParsePixels(client_t *client)
{
	int local_id = MSG_ReadShort();
	int offset = MSG_ReadLong();
	int len = MSG_ReadShort();
	sv_spray_upload_t *upload = &sv_spray_uploads[client - svs.clients];

	if (len < 0 || offset < 0 || !upload->active || !upload->accepted || offset + len > upload->byte_count || upload->local_id != local_id || offset != upload->received) {
		MSG_ReadSkip(max(0, len));
		SV_SpraySendDeny(client, local_id);
		memset(upload, 0, sizeof(*upload));
		return;
	}

	MSG_ReadData(upload->pixels + offset, len);
	upload->received = offset + len;

	if (upload->received == upload->byte_count) {
		unsigned long long hash = SV_SprayHashBytes(upload->pixels, upload->width, upload->height, upload->byte_count);
		sv_spray_image_t *cached;

		if (hash != upload->hash) {
			// The claimed cache key must describe the exact uploaded bytes. A
			// mismatch would poison the cache for every later placement.
			SV_SpraySendDeny(client, local_id);
			memset(upload, 0, sizeof(*upload));
			return;
		}

		cached = SV_SprayImageFind(hash);
		if (!cached) {
			// First time this image hash was accepted on the map/server cache.
			cached = SV_SprayImageAlloc(hash);
			cached->width = upload->width;
			cached->height = upload->height;
			cached->byte_count = upload->byte_count;
			memcpy(cached->pixels, upload->pixels, upload->byte_count);
		}

		// The uploading client has the normalized image locally, so it can also
		// receive future placement-only uses of this hash.
		SV_SprayClientRememberHash(client, hash);
		SV_SprayFinalize(client, upload, cached->pixels);
		memset(upload, 0, sizeof(*upload));
	}
}

void SV_SpraysParseClientMessage(client_t *client)
{
	int sub = MSG_ReadByte();

	switch (sub) {
	case spraynet_begin:
		SV_SpraysParseBegin(client);
		break;
	case spraynet_pixels:
		SV_SpraysParsePixels(client);
		break;
	default:
		Con_Printf("SV_SpraysParseClientMessage: unknown spray submessage %d\n", sub);
		SV_DropClient(client);
		break;
	}
}

#endif // MVD_PEXT1_SPRAYS
#endif // !CLIENTONLY
