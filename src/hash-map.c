#include "hash-map.h"

#include <stdint.h>
#include <stdlib.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

struct HashSet {
    size_t stride;
    size_t keyOffset;
    size_t capacity;
    size_t count;
    // 0 - free, 1 - occupied, 2 - deleted
    uint8_t *states;
    void *data;
};

static int hash_set_init(struct HashSet *set, size_t stride, size_t key_offset);
static void hash_set_terminate(struct HashSet *set);
static int hash_set_insert(struct HashSet *set, const void *data, size_t key_size);
static void *hash_set_get(struct HashSet *set, const void *key, size_t key_size);
static void hash_set_remove(struct HashSet *set, const void *key, size_t key_size);
static void hash_set_foreach_with_remove(struct HashSet *set, bool f(void *data, void *ctx), void *ctx, size_t key_size);

struct HashSetU32 {
    struct HashSet set;
};

struct HashSetU32 *hash_set_u32_create(size_t stride, size_t key_offset)
{
    struct HashSetU32 *set = malloc(sizeof(struct HashSetU32));
    if (set == NULL)
        return NULL;

    if (hash_set_init(&set->set, stride, key_offset) == -1) {
        free(set);
        return NULL;
    }

    return set;
}

void hash_set_u32_destroy(struct HashSetU32 *set)
{
    if (set != NULL)
        hash_set_terminate(&set->set);
    free(set);
}

int hash_set_u32_insert(struct HashSetU32 *set, const void *data)
{
    return hash_set_insert(&set->set, data, sizeof(uint32_t));
}

void *hash_set_u32_get(struct HashSetU32 *set, uint32_t key)
{
    return hash_set_get(&set->set, &key, sizeof(uint32_t));
}

void hash_set_u32_remove(struct HashSetU32 *set, uint32_t key)
{
    hash_set_remove(&set->set, &key, sizeof(uint32_t));
}

size_t hash_set_u32_size(struct HashSetU32 *set)
{
    return set->set.count;
}

void hash_set_u32_foreach(struct HashSetU32 *set, void f(void *data, void *ctx), void *ctx)
{
    for (size_t i = 0; i < set->set.capacity; i++) {
        if (set->set.states[i] == 1)
            f((char *)set->set.data + i * set->set.stride, ctx);
    }
}

void hash_set_u32_foreach_with_remove(struct HashSetU32 *set, bool f(void *data, void *ctx), void *ctx)
{
    hash_set_foreach_with_remove(&set->set, f, ctx, sizeof(uint32_t));
}

struct HashSetU16 {
    struct HashSet set;
};

struct HashSetU16 *hash_set_u16_create(size_t stride, size_t key_offset)
{
    struct HashSetU16 *set = malloc(sizeof(struct HashSetU16));
    if (set == NULL)
        return NULL;

    if (hash_set_init(&set->set, stride, key_offset) == -1) {
        free(set);
        return NULL;
    }

    return set;
}

void hash_set_u16_destroy(struct HashSetU16 *set)
{
    if (set != NULL)
        hash_set_terminate(&set->set);
    free(set);
}

int hash_set_u16_insert(struct HashSetU16 *set, const void *data)
{
    return hash_set_insert(&set->set, data, sizeof(uint16_t));
}

void *hash_set_u16_get(struct HashSetU16 *set, uint16_t key)
{
    return hash_set_get(&set->set, &key, sizeof(uint16_t));
}

void hash_set_u16_remove(struct HashSetU16 *set, uint16_t key)
{
    hash_set_remove(&set->set, &key, sizeof(uint16_t));
}

size_t hash_set_u16_size(struct HashSetU16 *set)
{
    return set->set.count;
}

void hash_set_u16_foreach(struct HashSetU16 *set, void f(void *data, void *ctx), void *ctx)
{
    for (size_t i = 0; i < set->set.capacity; i++) {
        if (set->set.states[i] == 1)
            f((char *)set->set.data + i * set->set.stride, ctx);
    }
}

struct HashSetAddr {
    struct HashSet set;
};

struct HashSetAddr *hash_set_addr_create(size_t stride, size_t key_offset)
{
    struct HashSetAddr *set = malloc(sizeof(struct HashSetAddr));
    if (set == NULL)
        return NULL;

    if (hash_set_init(&set->set, stride, key_offset) == -1) {
        free(set);
        return NULL;
    }

    return set;
}

void hash_set_addr_destroy(struct HashSetAddr *set)
{
    if (set != NULL)
        hash_set_terminate(&set->set);
    free(set);
}

int hash_set_addr_insert(struct HashSetAddr *set, const void *data)
{
    const struct sockaddr *addr = (const void *)((const char *)data + set->set.keyOffset);
    if (addr->sa_family == AF_INET)
        return hash_set_insert(&set->set, data, sizeof(struct sockaddr_in));
    else // For now only AF_INET and AF_INET6 are supported
        return hash_set_insert(&set->set, data, sizeof(struct sockaddr_in6));
}

void *hash_set_addr_get(struct HashSetAddr *set, struct sockaddr *addr)
{
    if (addr->sa_family == AF_INET)
        return hash_set_get(&set->set, addr, sizeof(struct sockaddr_in));
    else
        return hash_set_get(&set->set, addr, sizeof(struct sockaddr_in6));
}

void hash_set_addr_remove(struct HashSetAddr *set, struct sockaddr *addr)
{
    if (addr->sa_family == AF_INET)
        hash_set_remove(&set->set, addr, sizeof(struct sockaddr_in));
    else
        hash_set_remove(&set->set, addr, sizeof(struct sockaddr_in6));
}

size_t hash_set_addr_size(struct HashSetAddr *set)
{
    return set->set.count;
}

void hash_set_addr_foreach(struct HashSetAddr *set, void f(void *data, void *ctx), void *ctx)
{
    for (size_t i = 0; i < set->set.capacity; i++) {
        if (set->set.states[i] == 1)
            f((char *)set->set.data + i * set->set.stride, ctx);
    }
}

void hash_set_addr_foreach_with_remove(struct HashSetAddr *set, bool f(void *data, void *ctx), void *ctx)
{
    hash_set_foreach_with_remove(&set->set, f, ctx, sizeof(struct sockaddr_storage));
}

static int hash_set_init(struct HashSet *set, size_t stride, size_t key_offset)
{
    set->data = malloc(stride);
    if (set->data == NULL)
        return -1;

    set->states = malloc(sizeof(uint8_t));
    if (set->states == NULL) {
        free(set->data);
        return -1;
    }

    set->states[0] = 0;

    set->capacity =1;
    set->count = 0;
    set->stride = stride;
    set->keyOffset = key_offset;

    return 0;
}

static void hash_set_terminate(struct HashSet *set)
{
    free(set->states);
    free(set->data);
}

static int hash_set_insert(struct HashSet *set, const void *data, size_t key_size)
{
    if (set->count == set->capacity) {
        void *new = malloc((set->capacity * 2) * set->stride);
        if (new == NULL)
            return -1;

        uint8_t *new_states = calloc(set->capacity * 2, sizeof(uint8_t));
        if (new_states == NULL) {
            free(new);
            return -1;
        }

        for (size_t i = 0; i < set->capacity; i++) {
            size_t new_i = XXH3_64bits((char *)set->data + i * set->stride + set->keyOffset, key_size) % (set->capacity * 2);
            while (new_states[new_i] != 0) {
                new_i++;
                new_i %= set->capacity * 2;
            }

            new_states[new_i] = 1;
            memcpy((char *)new + new_i * set->stride, (char *)set->data + i * set->stride, set->stride);
        }

        free(set->states);
        free(set->data);
        set->data = new;
        set->states = new_states;
        set->capacity *= 2;
    }

    size_t index = XXH3_64bits((char *)data + set->keyOffset, key_size) % set->capacity;
    while (set->states[index] == 1) {
        index++;
        index %= set->capacity;
    }

    set->states[index] = 1;
    memcpy((char *)set->data + index * set->stride, data, set->stride);

    set->count++;

    return 0;
}

static void *hash_set_get(struct HashSet *set, const void *key, size_t key_size)
{
    size_t start = XXH3_64bits(key, key_size) % set->capacity;
    size_t index = start;
    while (set->states[index] == 2 || (set->states[index] == 1 && memcmp((char *)set->data + index * set->stride + set->keyOffset, key, key_size))) {
        index++;
        index %= set->capacity;
        if (index == start)
            return NULL;
    }

    if (set->states[index] == 0)
        return NULL;

    return (char *)set->data + index * set->stride;
}

static void hash_set_remove(struct HashSet *set, const void *key, size_t key_size)
{
    size_t index = XXH3_64bits(key, key_size) % set->capacity;
    while ((set->states[index] == 1 && memcmp((char *)set->data + index * set->stride + set->keyOffset, key, key_size)) || set->states[index] == 2) {
        index++;
        index %= set->capacity;
    }

    set->states[index] = 2;

    set->count--;

    if (set->count < set->capacity / 4) {
        void *new = malloc((set->capacity / 2) * set->stride);
        if (new == NULL)
            return;

        uint8_t *new_states = malloc((set->capacity / 2) * sizeof(uint8_t));
        if (new_states == NULL) {
            free(new);
            return;
        }

        for (size_t i = 0; i < set->capacity / 2; i++)
            new_states[i] = 0;

        for (size_t i = 0; i < set->capacity; i++) {
            if (set->states[i] == 1) {
                size_t new_i = XXH3_64bits((((char *)set->data + i * set->stride) + set->keyOffset), key_size) % (set->capacity / 2);
                while (new_states[new_i] != 0) {
                    new_i++;
                    new_i %= set->capacity / 2;
                }

                new_states[new_i] = 1;
                memcpy((char *)new + new_i * set->stride, (char *)set->data + i * set->stride, set->stride);
            }
        }

        free(set->data);
        free(set->states);
        set->data = new;
        set->states = new_states;
        set->capacity /= 2;
    }
}

static void hash_set_foreach_with_remove(struct HashSet *set, bool f(void *data, void *ctx), void *ctx, size_t key_size)
{
    for (size_t i = 0; i < set->capacity; i++) {
        if (set->states[i] == 1) {
            if (f((char *)set->data + i * set->stride, ctx)) {
                set->states[i] = 2;
                set->count--;
            }
        }
    }

    if (set->count < set->capacity / 4) {
        void *new = malloc((set->capacity / 2) * set->stride);
        if (new == NULL)
            return;

        uint8_t *new_states = malloc((set->capacity / 2) * sizeof(uint8_t));
        if (new_states == NULL) {
            free(new);
            return;
        }

        for (size_t i = 0; i < set->capacity / 2; i++)
            new_states[i] = 0;

        for (size_t i = 0; i < set->capacity; i++) {
            if (set->states[i] == 1) {
                size_t new_i = XXH3_64bits((((char *)set->data + i * set->stride) + set->keyOffset), key_size) % (set->capacity / 2);
                while (new_states[new_i] != 0) {
                    new_i++;
                    new_i %= set->capacity / 2;
                }

                new_states[new_i] = 1;
                memcpy((char *)new + new_i * set->stride, (char *)set->data + i * set->stride, set->stride);
            }
        }

        free(set->data);
        free(set->states);
        set->data = new;
        set->states = new_states;
        set->capacity /= 2;
    }
}

