#ifndef DATABASE_H
#define DATABASE_H

#include <stdbool.h>
#include <stdint.h>

#include <mysql.h>

#include "account.h"
#include "character.h"
#include "item.h"

#define ACCOUNT_HASH_LEN 16

struct DatabaseConnection;

enum DatabaseRequestType {
    DATABASE_REQUEST_TYPE_TRY_CREATE_ACCOUNT,
    DATABASE_REQUEST_TYPE_GET_ACCOUNT_CREDENTIALS,
    DATABASE_REQUEST_TYPE_GET_ACCOUNT,
    DATABASE_REQUEST_TYPE_UPDATE_ACCOUNT,
    DATABASE_REQUEST_TYPE_GET_CHARACTERS_FOR_ACCOUNT_FOR_WORLD,
    DATABASE_REQUEST_TYPE_GET_CHARACTERS_FOR_ACCOUNT,
    DATABASE_REQUEST_TYPE_GET_CHARACTER_EXISTS,
    DATABASE_REQUEST_TYPE_TRY_CREATE_CHARACTER,
    DATABASE_REQUEST_TYPE_GET_CHARACTER,
    DATABASE_REQUEST_TYPE_GET_MONSTER_DROPS,
    DATABASE_REQUEST_TYPE_UPDATE_CHARACTER
};

struct DatabaseItem {
    uint64_t id;
    uint32_t itemId;
    size_t ownerLength;
    char owner[CHARACTER_MAX_NAME_LENGTH];
    uint8_t flags;
    int64_t expiration;
    size_t giverLength;
    char giver[CHARACTER_MAX_NAME_LENGTH];
};

struct DatabaseEquipment {
    uint64_t id;
    struct DatabaseItem item;
    int8_t level;
    int8_t slots;
    int16_t str;
    int16_t dex;
    int16_t int_;
    int16_t luk;
    int16_t hp;
    int16_t mp;
    int16_t atk;
    int16_t matk;
    int16_t def;
    int16_t mdef;
    int16_t acc;
    int16_t avoid;
    int16_t hands;
    int16_t speed;
    int16_t jump;
};

struct DatabaseProgress {
    uint16_t questId;
    uint32_t progressId;
    int16_t progress;
};

struct DatabaseCompletedQuest {
    uint16_t id;
    MYSQL_TIME time;
};

struct RequestParams {
    enum DatabaseRequestType type;
    union {
        // DATABASE_REQUEST_TYPE_TRY_CREATE_ACCOUNT,
        struct {
            uint8_t nameLength;
            char name[ACCOUNT_NAME_MAX_LENGTH];
            uint8_t hash[ACCOUNT_HASH_LEN];
            uint64_t salt;
        } tryCreateAccount;
        // DATABASE_REQUEST_TYPE_GET_ACCOUNT_CREDENTIALS,
        struct {
            uint8_t nameLength;
            char name[ACCOUNT_NAME_MAX_LENGTH];
        } getAccountCredentials;
        struct {
            uint32_t id;
        } getAccount;
        struct {
            uint8_t picLength;
            char pic[ACCOUNT_PIC_MAX_LENGTH];
            uint32_t id;
            uint8_t tos;
            my_bool isGenderNull;
            uint8_t gender;
        } updateAccount;
        struct {
            uint8_t nameLength;
            char name[CHARACTER_MAX_NAME_LENGTH];
        } getCharacterExists;
        struct {
            uint8_t nameLength;
            char name[CHARACTER_MAX_NAME_LENGTH];
            uint32_t accountId;
            uint8_t world;
            uint32_t map;
            uint16_t job;
            uint8_t gender;
            uint8_t skin;
            uint32_t hair;
            uint32_t face;
            struct Equipment top;
            struct Equipment bottom;
            struct Equipment shoes;
            struct Equipment weapon;
        } tryCreateCharacter;
        struct {
            uint32_t id;
            uint8_t world;
        } getCharactersForAccountForWorld;
        struct {
            uint32_t id;
        } getCharactersForAccount;
        struct {
            uint32_t id;
        } getCharacter;
        struct {
            uint32_t id;
            uint32_t map;
            uint8_t spawnPoint;
            uint16_t job;
            uint8_t level;
            int32_t exp;
            int16_t maxHp;
            int16_t hp;
            int16_t maxMp;
            int16_t mp;
            int16_t str;
            int16_t dex;
            int16_t int_;
            int16_t luk;
            int16_t ap;
            int16_t sp;
            int16_t fame;
            uint8_t skin;
            uint32_t face;
            uint32_t hair;
            int32_t mesos;
            uint8_t equipSlots;
            uint8_t useSlots;
            uint8_t setupSlots;
            uint8_t etcSlots;
            size_t equippedCount;
            struct DatabaseEquipment equippedEquipment[EQUIP_SLOT_COUNT];
            size_t equipCount;
            struct {
                uint8_t slot;
                struct DatabaseEquipment equip;
            } equipmentInventory[252];
            size_t itemCount;
            struct {
                uint8_t slot;
                int16_t count;
                struct DatabaseItem item;
            } inventoryItems[4 * 252];
            size_t questCount;
            uint16_t *quests;
            size_t progressCount;
            struct DatabaseProgress *progresses;
            size_t completedQuestCount;
            struct DatabaseCompletedQuest *completedQuests;
        } updateCharacter;
    };
};

struct DatabaseDropData {
    uint32_t itemId;
    int32_t min;
    int32_t max;
    bool isQuest;
    uint16_t questId;
    int32_t chance;
};

struct ItemDrop {
    uint32_t itemId;
    int32_t chance;
};

struct MonsterItemDrops {
    size_t count;
    struct ItemDrop *drops;
};

struct QuestItemDrop {
    uint32_t itemId;
    uint16_t questId;
    int32_t chance;
};

struct MonsterQuestItemDrops {
    size_t count;
    struct QuestItemDrop *drops;
};

struct MesoDrop {
    int32_t min;
    int32_t max;
    int32_t chance;
};

struct MultiItemDrop {
    uint32_t id;
    int32_t min;
    int32_t max;
    int32_t chance;
};

struct MonsterMultiItemDrops {
    size_t count;
    struct MultiItemDrop *drops;
};

struct MonsterDrops {
    uint32_t id;
    struct MonsterItemDrops itemDrops;
    struct MonsterQuestItemDrops questItemDrops;
    struct MesoDrop mesoDrop;
    struct MonsterMultiItemDrops multiItemDrops;
};

union DatabaseResult {
    struct {
        bool created;
        uint32_t id;
    } tryCreateAccount;
    struct {
        uint32_t id;
        uint8_t hash[ACCOUNT_HASH_LEN];
        uint64_t salt;
        bool found;
    } getAccountCredentials;
    struct {
        size_t picLength;
        char pic[ACCOUNT_PIC_MAX_LENGTH];
        uint8_t tos;
        my_bool isGenderNull;
        uint8_t gender;
    } getAccount;
    struct {
        bool created;
        uint32_t id;
    } tryCreateCharacter;
    struct {
        bool exists;
    } getCharacterExists;
    struct {
        uint8_t characterCount;
        // SELECT (id, name, job, level, max_hp, max_mp, str, dex, int_, luk, ap, sp, fame, gender, skin, face, hair) FROM Characters WHERE account_id = ? AND world = ?
        struct {
            uint32_t id;
            size_t nameLength;
            char name[CHARACTER_MAX_NAME_LENGTH];
            uint16_t job;
            uint8_t level;
            int32_t exp;
            int16_t maxHp;
            int16_t hp;
            int16_t maxMp;
            int16_t mp;
            int16_t str;
            int16_t dex;
            int16_t int_;
            int16_t luk;
            int16_t ap;
            int16_t sp;
            int16_t fame;
            uint8_t gender;
            uint8_t skin;
            uint32_t face;
            uint32_t hair;
            uint8_t equipCount;
            uint32_t equipment[EQUIP_SLOT_COUNT];
        } characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD];
    } getCharactersForAccountForWorld;
    struct {
        size_t nameLength;
        char name[CHARACTER_MAX_NAME_LENGTH];
        uint32_t map;
        uint8_t spawnPoint;
        uint16_t job;
        uint8_t level;
        int32_t exp;
        int16_t maxHp;
        int16_t hp;
        int16_t maxMp;
        int16_t mp;
        int16_t str;
        int16_t dex;
        int16_t int_;
        int16_t luk;
        int16_t hpmp;
        int16_t ap;
        int16_t sp;
        int16_t fame;
        uint8_t gender;
        uint8_t skin;
        uint32_t face;
        uint32_t hair;
        int32_t mesos;
        uint8_t equipSlots;
        uint8_t useSlots;
        uint8_t setupSlots;
        uint8_t etcSlots;
        size_t equippedCount;
        struct DatabaseEquipment equippedEquipment[EQUIP_SLOT_COUNT];
        size_t equipCount;
        struct {
            uint8_t slot;
            struct DatabaseEquipment equip;
        } equipmentInventory[252];
        size_t itemCount;
        struct {
            uint8_t slot;
            int16_t count;
            struct DatabaseItem item;
        } inventoryItems[4 * 252];
        size_t questCount;
        uint16_t *quests;
        size_t progressCount;
        struct DatabaseProgress *progresses;
        size_t completedQuestCount;
        struct DatabaseCompletedQuest *completedQuests;
    } getCharacter;
    struct {
        size_t count;
        struct MonsterDrops *monsters;
    } getMonsterDrops;
};

void database_connection_set_credentials(char *host, char *user, char *password, char *db, uint16_t port, char *socket);

struct DatabaseConnection *database_connection_create(const char *host, const char *user, const char *password, const char *db, uint16_t port, const char *socket);
void database_connection_destroy(struct DatabaseConnection *conn);
int database_connection_get_fd(struct DatabaseConnection *conn);
int database_connection_lock(struct DatabaseConnection *conn);
int database_connection_unlock(struct DatabaseConnection *conn);

/**
 * Represents an execute/fetch request to the database.
 * A \p DatabaseRequrest starts in the Initial state.
 * Use \p database_request_execute() to transition a \p DatabaseRequrest to the Executing state.
 * The application will ensure that at most, only one \p DatabaseRequrest will be in the Executing state for a certain \p DatabaseConnection instance.
 * The application will then call \p database_request_execute() repeatedly with the single request that is in the Executing state until it returns 0.
 * Repeated calls to \p database_request_execute() will keep the \p DatabaseRequrest in the Executing state as long as it returns a positive value.
 * Once \p database_request_execute() returns
 */
struct DatabaseRequest;

/**
 * Creates a new database request that will be ready for execution in a later time.
 *
 * \param conn A valid \p DatabaseConnection.
 * \param params Parameters of the request describing what to fetch/flush.
 */
struct DatabaseRequest *database_request_create(struct DatabaseConnection *conn, const struct RequestParams *params);

/**
 * Executes a request.
 * The first time this function is called on a certain \p req, status is ignored and \p req is transitioned to the Exexuting state if a positive value is returned or the Finished state if 0 is returned.
 * If the returned value is positive then it is a bitmask of POLLIN, POLLPRI, POLLOUT indicating which event the application should wait for on the file descriptor returned by \p database_conntection_get_fd() before trying to call \p database_request_execute() again.
 * On subsequent calls to \p database_request_execute(), status is a bitmask of POLLIN, POLLPRI, POLLOUT indicating which event the file descriptor is ready for.
 * If a positive value is returned then \p req is still in the Executing state and \p database_request_execute() should be called again.
 * If 0 is returned then the execution finished and \p req is transitioned to the Finished state.
 *
 * \p req The database request. It must be in the Initial or Executing state.
 *
 * \returns 0 if the execution was successfully finished. A positive value if the execution was only partially completed.
 *  A negative value if an error occurred. The error code is a negation of a MySQL error code.
 */
int database_request_execute(struct DatabaseRequest *req, int status);

/**
 * Retrieves the result from the request.
 *
 * \p req The database request. It must be in the Finished state.
 *
 * \returns The database result. It is a reference inside \p req so as long as \p req is alive so is the result object
 */
const union DatabaseResult *database_request_result(struct DatabaseRequest *req);

void database_request_destroy(struct DatabaseRequest *req);

#endif

