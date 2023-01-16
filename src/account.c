#include "account.h"

#include <stdlib.h>
#include <threads.h>

mtx_t LOCK;

struct AccountNode {
    struct AccountNode *prev;
    struct AccountNode *next;
    uint32_t id;
    uint32_t token;
} *HEAD;

struct Account account_get_default_account(uint8_t name_len, char *name)
{
    return (struct Account) {
        .nameLength = name_len,
        .picLength = 0,
        .tos = false,
        .gender = ACCOUNT_GENDER_UNSPECIFIED
    };
}

int accounts_init()
{
    return mtx_init(&LOCK, mtx_plain);
}

struct AccountNode *account_login(uint32_t id)
{
    // Allocate the new node before locking so that the lock will will be released faster
    struct AccountNode *new = malloc(sizeof(struct AccountNode));
    if (new == NULL)
        return NULL;

    mtx_lock(&LOCK);
    struct AccountNode *temp = HEAD;
    while (temp != NULL) {
        if (temp->id == id) {
            mtx_unlock(&LOCK);
            free(new);
            return NULL;
        }
        temp = temp->next;
    }

    new->id = id;
    new->next = HEAD;
    new->prev = NULL;
    if (HEAD != NULL)
        HEAD->prev = new;
    HEAD = new;

    mtx_unlock(&LOCK);
    return new;
}

uint32_t account_get_id(struct AccountNode *account)
{
    return account->id;
}

void account_logout(struct AccountNode **node_)
{
    struct AccountNode *node = *node_;
    mtx_lock(&LOCK);
    if (node->prev != NULL)
        node->prev->next = node->next;
    else
        HEAD = node->next;

    if (node->next != NULL)
        node->next->prev = node->prev;

    mtx_unlock(&LOCK);

    free(node);
    *node_ = NULL;
}

void account_set_token(struct AccountNode *node, uint32_t token)
{
    node->token = token;
}

void account_logout_by_token(uint32_t token)
{
    mtx_lock(&LOCK);
    struct AccountNode *node = HEAD;
    while (node != NULL) {
        if (node->token == token)
            break;

        node = node->next;
    }

    if (node == NULL) {
        mtx_unlock(&LOCK);
        return;
    }

    if (node->prev != NULL)
        node->prev->next = node->next;
    else
        HEAD = node->next;

    if (node->next != NULL)
        node->next->prev = node->prev;

    mtx_unlock(&LOCK);

    free(node);
}

