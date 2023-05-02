#include "party.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <threads.h>

#include "hash-map.h"

atomic_uint RUNNING_ID = 1;

mtx_t PARTIES_MTX;
struct HashSetU32 *PARTIES;

struct Party {
    uint32_t id;
    uint32_t leader;
    uint8_t memberCount;
    uint32_t members[5];
};

struct PartyStorage {
    uint32_t id;
    struct Party *party;
};

int parties_init(void)
{
    if (mtx_init(&PARTIES_MTX, mtx_plain) == -1)
        return -1;

    PARTIES = hash_set_u32_create(sizeof(struct PartyStorage), offsetof(struct PartyStorage, id));
    if (PARTIES == NULL) {
        mtx_destroy(&PARTIES_MTX);
        return -1;
    }

    return 0;
}

void parties_terminate(void)
{
    hash_set_u32_destroy(PARTIES);
    mtx_destroy(&PARTIES_MTX);
}


struct Party *get_party_by_id(uint32_t id)
{
    mtx_lock(&PARTIES_MTX);
    struct Party *ret = hash_set_u32_get(PARTIES, id);
    mtx_unlock(&PARTIES_MTX);

    return ret;
}

struct Party *party_create(uint32_t leader)
{
    struct Party *party = malloc(sizeof(struct Party));
    if (party == NULL)
        return NULL;

    party->id = atomic_fetch_add(&RUNNING_ID, 1);
    party->leader = leader;
    party->memberCount = 0;

    struct PartyStorage new = {
        .id = party->id,
        .party = party,
    };

    mtx_lock(&PARTIES_MTX);
    hash_set_u32_insert(PARTIES, &new);
    mtx_unlock(&PARTIES_MTX);

    return party;
}

void party_destroy(struct Party *party)
{
    mtx_lock(&PARTIES_MTX);
    hash_set_u32_remove(PARTIES, party->id);
    mtx_unlock(&PARTIES_MTX);
}

uint32_t party_get_id(struct Party *party)
{
    return party->id;
}

uint32_t party_get_leader_id(struct Party *party)
{
    return party->leader;
}

void party_set_leader_id(struct Party *party, uint32_t id)
{
    party->leader = id;
}


