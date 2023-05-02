#ifndef PARTY_H
#define PARTY_H

#include <stdint.h>

struct Party;

int parties_init(void);
void parties_terminate(void);
struct Party *get_party_by_id(uint32_t);

struct Party *party_create(uint32_t leader);
void party_destroy(struct Party *party);
uint32_t party_get_id(struct Party *);
uint32_t party_get_leader_id(struct Party *);
void party_set_leader_id(struct Party *, uint32_t id);

#endif

