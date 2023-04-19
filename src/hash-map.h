#ifndef SYRUP_HASH_MAP_H
#define SYRUP_HASH_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>

struct HashSetU32;
struct HashSetU32 *hash_set_u32_create(size_t stride, size_t key_offset);
void hash_set_u32_destroy(struct HashSetU32 *set);
int hash_set_u32_insert(struct HashSetU32 *map, const void *data);
void *hash_set_u32_get(struct HashSetU32 *set, uint32_t key);
void hash_set_u32_remove(struct HashSetU32 *set, uint32_t key);
size_t hash_set_u32_size(struct HashSetU32 *set);
void hash_set_u32_foreach(struct HashSetU32 *set, void f(void *data, void *ctx), void *ctx);
// Iterates over the elements and for each one decide whether to delete that element or keep it
void hash_set_u32_foreach_with_remove(struct HashSetU32 *set, bool f(void *data, void *ctx), void *ctx);

struct HashSetU16;
struct HashSetU16 *hash_set_u16_create(size_t stride, size_t key_offset);
void hash_set_u16_destroy(struct HashSetU16 *set);
int hash_set_u16_insert(struct HashSetU16 *set, const void *data);
void *hash_set_u16_get(struct HashSetU16 *set, uint16_t key);
void hash_set_u16_remove(struct HashSetU16 *set, uint16_t key);
size_t hash_set_u16_size(struct HashSetU16 *set);
void hash_set_u16_foreach(struct HashSetU16 *set, void f(void *data, void *ctx), void *ctx);

struct HashSetAddr;
struct HashSetAddr *hash_set_addr_create(size_t stride, size_t key_offset);
void hash_set_addr_destroy(struct HashSetAddr *set);
int hash_set_addr_insert(struct HashSetAddr *set, const void *data);
void *hash_set_addr_get(struct HashSetAddr *set, struct sockaddr *key);
void hash_set_addr_remove(struct HashSetAddr *set, struct sockaddr *key);
size_t hash_set_addr_size(struct HashSetAddr *set);
void hash_set_addr_foreach(struct HashSetAddr *set, void f(void *data, void *ctx), void *ctx);
void hash_set_addr_foreach_with_remove(struct HashSetAddr *set, bool f(void *data, void *ctx), void *ctx);

#endif

