#ifndef ACCOUNT_H
#define ACCOUNT_H

#define ACCOUNT_NAME_MAX_LENGTH 12
#define ACCOUNT_PASSWORD_MAX_LENGTH 12
#define ACCOUNT_PIC_MAX_LENGTH 16
#define ACCOUNT_HWID_LENGTH 10
#define ACCOUNT_MAX_CHARACTERS_PER_WORLD 6

#include <stdbool.h>
#include <stdint.h>

enum AccountGender {
    ACCOUNT_GENDER_MALE,
    ACCOUNT_GENDER_FEMALE,
    ACCOUNT_GENDER_UNSPECIFIED = 10,
};

struct Account {
    uint8_t nameLength;
    char name[ACCOUNT_NAME_MAX_LENGTH];
    uint8_t picLength;
    char pic[ACCOUNT_PIC_MAX_LENGTH];
    bool tos;
    enum AccountGender gender;
};

struct Account account_get_default_account(uint8_t name_len, char *name);

struct AccountNode;

int accounts_init(void);
struct AccountNode *account_login(uint32_t id);
uint32_t account_get_id(struct AccountNode *account);
void account_logout(struct AccountNode **account);
void account_set_cid(struct AccountNode *account, uint32_t id);
void account_logout_by_cid(uint32_t id);

#endif

