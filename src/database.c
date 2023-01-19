#include "database.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <errmsg.h>
#include <mysql.h>

struct LockQueueNode {
    struct LockQueueNode *next;
    int value;
};

struct LockQueue {
    struct LockQueueNode *head;
    struct LockQueueNode *last;
};

static void lock_queue_init(struct LockQueue *queue);
static int lock_queue_enqueue(struct LockQueue *queue, int value);
static int lock_queue_dequeue(struct LockQueue *queue);
static bool lock_queue_empty(struct LockQueue *queue);

struct DatabaseConnection {
    MYSQL *conn;
    struct LockQueue queue;
};

struct DatabaseRequest {
    struct RequestParams params;
    int state;
    bool running;
    MYSQL_STMT *stmt;
    union DatabaseResult res;

    // Temporary data that needs to live between execute() calls used for character creation
    union {
        struct {
            my_bool isPicNull;
        };
        struct {
            uint64_t id[4];
        };
        struct {
            size_t i;
            my_bool isNull;
            union {
                struct {
                    uint16_t quest;
                    union {
                        struct {
                            uint32_t progressId;
                            int16_t progress;
                        };
                        MYSQL_TIME time;
                    };
                };
                struct {
                    uint32_t skillId;
                    int8_t skillLevel;
                    int8_t skillMasterLevel;
                };
            };
        } getCharacter;
        struct {
            size_t i;
            my_bool isNull;
            void *data;
        } updateCharacter;
        struct {
            struct DatabaseDropData drop;
        };
    } temp;
};

// MariaDB (for some reason) switches between POLLOUT and POLLPRI values so a conversion is needed
static int mariadb_to_poll(int status);
static int poll_to_mariadb(int status);

struct DatabaseConnection *database_connection_create(const char *host, const char *user, const char *password, const char *db, uint16_t port, const char *socket)
{
    struct DatabaseConnection *conn = malloc(sizeof(struct DatabaseConnection));
    if (conn == NULL)
        return NULL;

    conn->conn = mysql_init(NULL);
    if (conn->conn == NULL) {
        free(conn);
        return NULL;
    }

    mysql_options(conn->conn, MYSQL_OPT_NONBLOCK, 0);
    if (mysql_real_connect(conn->conn, host, user, password, db, port, socket, 0) == NULL) {
        mysql_close(conn->conn);
        free(conn);
        return NULL;
    }

    lock_queue_init(&conn->queue);

    return conn;
}

extern void database_connection_destroy(struct DatabaseConnection *conn)
{
    if (conn != NULL)
        mysql_close(conn->conn);
    free(conn);
}

int database_connection_get_fd(struct DatabaseConnection *conn)
{
    return mysql_get_socket(conn->conn);
}

int database_connection_lock(struct DatabaseConnection *conn)
{
    if (lock_queue_empty(&conn->queue)) {
        if (lock_queue_enqueue(&conn->queue, -1) == -1)
            return -1;

        return -2;
    }

    int value = eventfd(0, 0);
    if (value == -1) {
        return -1;
    }

    if (lock_queue_enqueue(&conn->queue, value) == -1) {
        close(value);
        return -1;
    }

    return value;
}

int database_connection_unlock(struct DatabaseConnection *conn)
{
    uint64_t one = 1;
    return write(lock_queue_dequeue(&conn->queue), &one, sizeof(uint64_t));
}

struct DatabaseRequest *database_request_create(struct DatabaseConnection *conn, const struct RequestParams *params)
{
    struct DatabaseRequest *req = malloc(sizeof(struct DatabaseRequest));
    if (req == NULL)
        return NULL;

    req->stmt = mysql_stmt_init(conn->conn);
    if (req->stmt == NULL) {
        free(req);
        return NULL;
    }

    req->state = 0;
    req->running = false;
    req->params = *params;

    if (req->params.type == DATABASE_REQUEST_TYPE_GET_MONSTER_DROPS) {
        req->res.getMonsterDrops.count = 0;
        req->res.getMonsterDrops.monsters = NULL;
    } else if (req->params.type == DATABASE_REQUEST_TYPE_GET_CHARACTER) {
        req->res.getCharacter.quests = NULL;
        req->res.getCharacter.progresses = NULL;
        req->res.getCharacter.completedQuests = NULL;
        req->res.getCharacter.skills = NULL;
    }

    return req;
}

void database_request_destroy(struct DatabaseRequest *req)
{
    mysql_stmt_close(req->stmt);
    if (req->params.type == DATABASE_REQUEST_TYPE_GET_MONSTER_DROPS) {
        for (size_t i = 0; i < req->res.getMonsterDrops.count; i++) {
            struct MonsterDrops *monster = &req->res.getMonsterDrops.monsters[i];
            free(monster->itemDrops.drops);
            free(monster->questItemDrops.drops);
            free(monster->multiItemDrops.drops);
        }
        free(req->res.getMonsterDrops.monsters);
    } else if (req->params.type == DATABASE_REQUEST_TYPE_GET_CHARACTER) {
        free(req->res.getCharacter.skills);
        free(req->res.getCharacter.completedQuests);
        free(req->res.getCharacter.progresses);
        free(req->res.getCharacter.quests);
    }

    free(req);
}

struct InputBinder {
    size_t count;
    size_t pos;
    MYSQL_BIND *bind;
};

static void input_binder_init(size_t count, MYSQL_BIND *bind, struct InputBinder *binder)
{
    binder->count = count;
    binder->pos = 0;
    binder->bind = bind;
}

#define INPUT_BINDER_INIT(count) \
    do { \
        MYSQL_BIND bind__[(count)]; \
        memset(bind__, 0, (count) * sizeof(MYSQL_BIND)); \
        struct InputBinder binder__; \
        input_binder_init((count), bind__, &binder__);


#define INPUT_BINDER_FINALIZE(stmt) \
        mysql_stmt_bind_param((stmt), bind__); \
    } while(0);

static void input_binder_bind(struct InputBinder *binder, enum enum_field_types type, bool is_unsigned, size_t size, void *data, char *ind)
{
    assert(binder->pos < binder->count);
    binder->bind[binder->pos].buffer_type = type;
    binder->bind[binder->pos].buffer = data;
    binder->bind[binder->pos].buffer_length = size;
    binder->bind[binder->pos].length = NULL;
    binder->bind[binder->pos].length_value = size;
    binder->bind[binder->pos].is_unsigned = is_unsigned ? 1 : 0;
    binder->bind[binder->pos].is_null = NULL;
    binder->bind[binder->pos].is_null_value = 0;
    binder->bind[binder->pos].u.indicator = ind;
    binder->pos++;
}

static void input_binder_bind_bulk(struct InputBinder *binder, enum enum_field_types type, bool is_unsigned, size_t *size, void *data, char *ind)
{
    assert(binder->pos < binder->count);
    binder->bind[binder->pos].buffer_type = type;
    binder->bind[binder->pos].buffer = data;
    binder->bind[binder->pos].buffer_length = *size;
    binder->bind[binder->pos].length = size;
    binder->bind[binder->pos].is_unsigned = is_unsigned ? 1 : 0;
    binder->bind[binder->pos].is_null = NULL;
    binder->bind[binder->pos].is_null_value = 0;
    binder->bind[binder->pos].u.indicator = ind;
    binder->pos++;
}

static void input_binder_bind_null(struct InputBinder *binder, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_NULL, false, 0, NULL, ind);
}
#define INPUT_BINDER_null() (input_binder_bind_null(&binder__, NULL))
#define INPUT_BINDER_bulk_null(ind) (input_binder_bind_null(&binder__), ind)

static void input_binder_bind_i8(struct InputBinder *binder, int8_t *data, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_TINY, false, 1, data, ind);
}
#define INPUT_BINDER_i8(data) (input_binder_bind_i8(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_i8(data, ind) (input_binder_bind_i8(&binder__, (data), ind))

static void input_binder_bind_u8(struct InputBinder *binder, uint8_t *data, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_TINY, true, 1, data, ind);
}
#define INPUT_BINDER_u8(data) (input_binder_bind_u8(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_u8(data, ind) (input_binder_bind_u8(&binder__, (data), ind))

static void input_binder_bind_i16(struct InputBinder *binder, int16_t *data, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_SHORT, false, 2, data, ind);
}
#define INPUT_BINDER_i16(data) (input_binder_bind_i16(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_i16(data, ind) (input_binder_bind_i16(&binder__, (data), ind))

static void input_binder_bind_u16(struct InputBinder *binder, uint16_t *data, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_SHORT, true, 2, data, ind);
}
#define INPUT_BINDER_u16(data) (input_binder_bind_u16(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_u16(data, ind) (input_binder_bind_u16(&binder__, (data), NULL))

static void input_binder_bind_i32(struct InputBinder *binder, int32_t *data, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_LONG, false, 4, data, ind);
}
#define INPUT_BINDER_i32(data) (input_binder_bind_i32(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_i32(data, ind) (input_binder_bind_i32(&binder__, (data), ind))

static void input_binder_bind_u32(struct InputBinder *binder, uint32_t *data, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_LONG, true, 4, data, ind);
}
#define INPUT_BINDER_u32(data) (input_binder_bind_u32(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_u32(data, ind) (input_binder_bind_u32(&binder__, (data), ind))

static void input_binder_bind_u64(struct InputBinder *binder, uint64_t *data, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_LONGLONG, true, 8, data, ind);
}
#define INPUT_BINDER_u64(data) (input_binder_bind_u64(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_u64(data, ind) (input_binder_bind_u64(&binder__, (data), ind))

static void input_binder_bind_string(struct InputBinder *binder, size_t size, char *string)
{
    input_binder_bind(binder, MYSQL_TYPE_STRING, false, size, string, NULL);
}
#define INPUT_BINDER_sized_string(size, data) (input_binder_bind_string(&binder__, (size), (data)))
#define INPUT_BINDER_string(data) (input_binder_bind_string(&binder__, strlen(data), (data)))

static void input_binder_bind_string_bulk(struct InputBinder *binder, unsigned long *size, char *string, char *ind)
{
    input_binder_bind_bulk(binder, MYSQL_TYPE_STRING, false, size, string, ind);
}
#define INPUT_BINDER_bulk_sized_string(data, size, ind) (input_binder_bind_string_bulk(&binder__, (size), (data), ind))

static void input_binder_bind_array(struct InputBinder *binder, size_t size, uint8_t *array)
{
    input_binder_bind(binder, MYSQL_TYPE_BLOB, false, size, array, NULL);
}
#define INPUT_BINDER_array(size, data) (input_binder_bind_array(&binder__, (size), (data)))

static void input_binder_bind_time(struct InputBinder *binder, MYSQL_TIME *time, char *ind)
{
    input_binder_bind(binder, MYSQL_TYPE_TIMESTAMP, false, sizeof(MYSQL_TIME), time, ind);
}
#define INPUT_BINDER_time(data) (input_binder_bind_time(&binder__, (data), NULL))
#define INPUT_BINDER_bulk_time(data, ind) (input_binder_bind_time(&binder__, (data), ind))

struct OutputBinder {
    size_t count;
    size_t pos;
    MYSQL_BIND *bind;
};

static void output_binder_init(size_t count, MYSQL_BIND *bind, struct OutputBinder *binder)
{
    binder->count = count;
    binder->pos = 0;
    binder->bind = bind;
}

#define OUTPUT_BINDER_INIT(count) \
    do { \
        MYSQL_BIND bind__[(count)]; \
        memset(bind__, 0, (count) * sizeof(MYSQL_BIND)); \
        struct OutputBinder binder__; \
        output_binder_init((count), (bind__), &binder__);


#define OUTPUT_BINDER_FINALIZE(stmt) \
        mysql_stmt_bind_result((stmt), bind__); \
    } while(0);

static void output_binder_bind(struct OutputBinder *binder, enum enum_field_types type, bool is_unsigned, size_t size, void *data, my_bool *is_null)
{
    assert(binder->pos < binder->count);
    binder->bind[binder->pos].buffer_type = type;
    binder->bind[binder->pos].buffer = data;
    binder->bind[binder->pos].buffer_length = size;
    binder->bind[binder->pos].length = NULL;
    binder->bind[binder->pos].length_value = size;
    binder->bind[binder->pos].is_unsigned = is_unsigned ? 1 : 0;
    binder->bind[binder->pos].is_null = is_null;

    binder->pos++;
}

static void output_binder_bind_with_size(struct OutputBinder *binder, enum enum_field_types type, bool is_unsigned, size_t *size, void *data, my_bool *is_null)
{
    assert(binder->pos < binder->count);
    binder->bind[binder->pos].buffer_type = type;
    binder->bind[binder->pos].buffer = data;
    binder->bind[binder->pos].buffer_length = *size;
    binder->bind[binder->pos].length = size;
    binder->bind[binder->pos].is_unsigned = is_unsigned ? 1 : 0;
    binder->bind[binder->pos].is_null = is_null;

    binder->pos++;
}

static void output_binder_bind_bool(struct OutputBinder *binder, bool *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_TINY, true, 1, out, is_null);
}
#define OUTPUT_BINDER_nullable_bool(out, is_null) (output_binder_bind_bool(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_bool(out) (output_binder_bind_bool(&binder__, (out), NULL))

static void output_binder_bind_i8(struct OutputBinder *binder, int8_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_TINY, false, 1, out, is_null);
}
#define OUTPUT_BINDER_nullable_i8(out, is_null) (output_binder_bind_i8(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_i8(out) (output_binder_bind_i8(&binder__, (out), NULL))

static void output_binder_bind_u8(struct OutputBinder *binder, uint8_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_TINY, true, 1, out, is_null);
}
#define OUTPUT_BINDER_nullable_u8(out, is_null) (output_binder_bind_u8(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_u8(out) (output_binder_bind_u8(&binder__, (out), NULL))

static void output_binder_bind_i16(struct OutputBinder *binder, int16_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_SHORT, false, 2, out, is_null);
}
#define OUTPUT_BINDER_nullable_i16(out, is_null) (output_binder_bind_i16(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_i16(out) (output_binder_bind_i16(&binder__, (out), NULL))

static void output_binder_bind_u16(struct OutputBinder *binder, uint16_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_SHORT, true, 2, out, is_null);
}
#define OUTPUT_BINDER_nullable_u16(out, is_null) (output_binder_bind_u16(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_u16(out) (output_binder_bind_u16(&binder__, (out), NULL))

static void output_binder_bind_i32(struct OutputBinder *binder, int32_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_LONG, false, 4, out, is_null);
}
#define OUTPUT_BINDER_nullable_i32(out, is_null) (output_binder_bind_i32(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_i32(out) (output_binder_bind_i32(&binder__, (out), NULL))

static void output_binder_bind_u32(struct OutputBinder *binder, uint32_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_LONG, true, 4, out, is_null);
}
#define OUTPUT_BINDER_nullable_u32(out, is_null) (output_binder_bind_u32(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_u32(out) (output_binder_bind_u32(&binder__, (out), NULL))

static void output_binder_bind_u64(struct OutputBinder *binder, uint64_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_LONGLONG, true, 8, out, is_null);
}
#define OUTPUT_BINDER_nullable_u64(out, is_null) (output_binder_bind_u64(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_u64(out) (output_binder_bind_u64(&binder__, (out), NULL))

static void output_binder_bind_array(struct OutputBinder *binder, size_t size, uint8_t *out, my_bool *is_null)
{
    output_binder_bind(binder, MYSQL_TYPE_BLOB, false, size, out, is_null);
}
#define OUTPUT_BINDER_nullable_array(size, out, is_null) (output_binder_bind_array(&binder__, (size), (out), (is_null)))
#define OUTPUT_BINDER_array(size, out) (OUTPUT_BINDER_nullable_array((size), (out), NULL))

static void output_binder_bind_sized_string(struct OutputBinder *binder, size_t *size, char *out, my_bool *is_null)
{
    output_binder_bind_with_size(binder, MYSQL_TYPE_STRING, false, size, out, is_null);
}
#define OUTPUT_BINDER_nullable_sized_string(size, out, is_null) (output_binder_bind_sized_string(&binder__, (size), (out), (is_null)))
#define OUTPUT_BINDER_sized_string(size, out) (OUTPUT_BINDER_nullable_sized_string((size), (out), NULL))

static void output_binder_bind_time(struct OutputBinder *binder, MYSQL_TIME *time, my_bool *is_null)
{
    assert(binder->pos < binder->count);
    output_binder_bind(binder, MYSQL_TYPE_TIMESTAMP, false, sizeof(MYSQL_TIME), time, is_null);

    binder->pos++;
}
#define OUTPUT_BINDER_nullable_time(out, is_null) (output_binder_bind_time(&binder__, (out), (is_null)))
#define OUTPUT_BINDER_time(out) (output_binder_bind_time(&binder__, (out), NULL))

static int do_try_create_account(struct DatabaseRequest *req, int status);
static int do_get_account_credentials(struct DatabaseRequest *req, int status);
static int do_get_account(struct DatabaseRequest *req, int status);
static int do_update_account(struct DatabaseRequest *req, int status);
static int do_get_characters_for_account_for_world(struct DatabaseRequest *req, int status);
static int do_get_characters_for_account(struct DatabaseRequest *req, int status);
static int do_get_characters_exists(struct DatabaseRequest *req, int status);
static int do_try_create_character(struct DatabaseRequest *req, int status);
static int do_get_character(struct DatabaseRequest *req, int status);
static int do_get_monster_drops(struct DatabaseRequest *req, int status);
static int do_update_character(struct DatabaseRequest *req, int status);

int database_request_execute(struct DatabaseRequest *req, int status)
{
    switch (req->params.type) {
        case DATABASE_REQUEST_TYPE_TRY_CREATE_ACCOUNT:
            return do_try_create_account(req, status);

        case DATABASE_REQUEST_TYPE_GET_ACCOUNT_CREDENTIALS:
            return do_get_account_credentials(req, status);

        case DATABASE_REQUEST_TYPE_GET_ACCOUNT:
            return do_get_account(req, status);

        case DATABASE_REQUEST_TYPE_UPDATE_ACCOUNT:
            return do_update_account(req, status);

        case DATABASE_REQUEST_TYPE_GET_CHARACTERS_FOR_ACCOUNT_FOR_WORLD:
            return do_get_characters_for_account_for_world(req, status);

        case DATABASE_REQUEST_TYPE_GET_CHARACTERS_FOR_ACCOUNT:
            return do_get_characters_for_account(req, status);

        case DATABASE_REQUEST_TYPE_GET_CHARACTER_EXISTS:
            return do_get_characters_exists(req, status);

        case DATABASE_REQUEST_TYPE_TRY_CREATE_CHARACTER:
            return do_try_create_character(req, status);

        case DATABASE_REQUEST_TYPE_GET_CHARACTER:
            return do_get_character(req, status);

        case DATABASE_REQUEST_TYPE_GET_MONSTER_DROPS:
            return do_get_monster_drops(req, status);

        case DATABASE_REQUEST_TYPE_UPDATE_CHARACTER:
            return do_update_character(req, status);

        default:
            assert(false);
    }
}

const union DatabaseResult *database_request_result(struct DatabaseRequest *req)
{
    return &req->res;
}

#define BEGIN_ASYNC(req) \
    switch((req)->state) { \
    default:

#define END_ASYNC() \
    }

#define DO_ASYNC(ret, failure, func, req, status, ...) \
    do { \
    (req)->state = __LINE__; case __LINE__: \
        if (!(req)->running) { \
            int status; \
            if ((status = func##_start(&(ret), (req)->stmt, ## __VA_ARGS__)) != 0) { \
                (req)->running = true; \
                return mariadb_to_poll(status); \
            } \
            if (failure) { \
                fprintf(stderr, "MySQL error at line %d: %s\n", __LINE__, mysql_stmt_error((req)->stmt)); \
                return -mysql_stmt_errno((req)->stmt); \
            } \
        } else { \
            if ((status = func##_cont(&ret, (req)->stmt, poll_to_mariadb(status))) != 0) \
                return mariadb_to_poll(status); \
            if (failure) { \
                fprintf(stderr, "MySQL error at line %d: %s\n", __LINE__, mysql_stmt_error((req)->stmt)); \
                return -mysql_stmt_errno((req)->stmt); \
            } \
            (req)->running = false; \
        } \
    } while (0)

static int do_try_create_account(struct DatabaseRequest *req, int status)
{
    int ret;
    const char *query = "INSERT IGNORE INTO Accounts (name, hash, salt) VALUES (?, ?, ?)";
    BEGIN_ASYNC(req)
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(3);
    INPUT_BINDER_sized_string(req->params.tryCreateAccount.nameLength, req->params.tryCreateAccount.name);
    INPUT_BINDER_array(ACCOUNT_HASH_LEN, req->params.tryCreateAccount.hash);
    INPUT_BINDER_u64(&req->params.tryCreateAccount.salt);
    INPUT_BINDER_FINALIZE(req->stmt);
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    req->res.tryCreateAccount.created = mysql_stmt_affected_rows(req->stmt) == 1;
    if (!req->res.tryCreateAccount.created)
        return 0;

    req->res.tryCreateAccount.id = mysql_stmt_insert_id(req->stmt);
    END_ASYNC()

    return 0;
}

static int do_get_account_credentials(struct DatabaseRequest *req, int status)
{
    int ret;
    const char *query = "SELECT id, hash, salt FROM Accounts WHERE name = ?";
    BEGIN_ASYNC(req)
        DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_sized_string(req->params.getAccountCredentials.nameLength, req->params.getAccountCredentials.name);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(3);
    OUTPUT_BINDER_u32(&req->res.getAccountCredentials.id);
    OUTPUT_BINDER_array(ACCOUNT_HASH_LEN, req->res.getAccountCredentials.hash);
    OUTPUT_BINDER_u64(&req->res.getAccountCredentials.salt);
    OUTPUT_BINDER_FINALIZE(req->stmt);
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);

    req->res.getAccountCredentials.found = ret != MYSQL_NO_DATA;
    END_ASYNC()

    return 0;
}

static int do_get_account(struct DatabaseRequest *req, int status)
{
    int ret;
    const char *query = "SELECT pic, tos, gender FROM Accounts WHERE id = ?";
    BEGIN_ASYNC(req);
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getAccount.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(3);
    req->res.getAccount.picLength = ACCOUNT_PIC_MAX_LENGTH;
    OUTPUT_BINDER_nullable_sized_string(&req->res.getAccount.picLength, req->res.getAccount.pic, &req->temp.isPicNull);
    OUTPUT_BINDER_u8(&req->res.getAccount.tos);
    OUTPUT_BINDER_nullable_u8(&req->res.getAccount.gender, &req->res.getAccount.isGenderNull);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    END_ASYNC()

    if (req->temp.isPicNull)
        req->res.getAccount.picLength = 0;

    return 0;
}

static int do_update_account(struct DatabaseRequest *req, int status)
{
    int ret;
        const char *query = "UPDATE Accounts SET pic = ?, tos = ?, gender = ? WHERE id = ?";
    BEGIN_ASYNC(req)
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(4);
    if (req->params.updateAccount.picLength == 0)
        INPUT_BINDER_null();
    else
        INPUT_BINDER_sized_string(req->params.updateAccount.picLength, req->params.updateAccount.pic);

    INPUT_BINDER_u8(&req->params.updateAccount.tos);
    if (req->params.updateAccount.isGenderNull)
        INPUT_BINDER_null();
    else
        INPUT_BINDER_u8(&req->params.updateAccount.gender);
    INPUT_BINDER_u32(&req->params.updateAccount.id);
    INPUT_BINDER_FINALIZE(req->stmt);
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    END_ASYNC()

    return 0;
}

static int do_get_characters_for_account_for_world(struct DatabaseRequest *req, int status)
{
    int ret;
    my_bool bret;
    const char *query = "SELECT id, name, job, level, exp, max_hp, hp, max_mp, mp, str, dex, int_, luk, ap, sp, fame, gender, skin, face, hair FROM Characters WHERE account_id = ? AND world = ?";
    BEGIN_ASYNC(req)
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(2);
    INPUT_BINDER_u32(&req->params.getCharactersForAccountForWorld.id);
    INPUT_BINDER_u8(&req->params.getCharactersForAccountForWorld.world);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(20);
    OUTPUT_BINDER_u32(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].id);
    req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].nameLength = CHARACTER_MAX_NAME_LENGTH;
    OUTPUT_BINDER_sized_string(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].nameLength, req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].name);
    OUTPUT_BINDER_u16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].job);
    OUTPUT_BINDER_u8(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].level);
    OUTPUT_BINDER_i32(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].exp);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].maxHp);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].hp);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].maxMp);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].mp);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].str);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].dex);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].int_);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].luk);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].ap);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].sp);
    OUTPUT_BINDER_i16(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].fame);
    OUTPUT_BINDER_u8(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].gender);
    OUTPUT_BINDER_u8(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].skin);
    OUTPUT_BINDER_u32(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].face);
    OUTPUT_BINDER_u32(&req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1].hair);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    req->res.getCharactersForAccountForWorld.characterCount = 0;
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    while (ret != MYSQL_NO_DATA) {
        req->res.getCharactersForAccountForWorld.characters[req->res.getCharactersForAccountForWorld.characterCount] = req->res.getCharactersForAccountForWorld.characters[ACCOUNT_MAX_CHARACTERS_PER_WORLD - 1];
        req->res.getCharactersForAccountForWorld.characterCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    if (req->res.getCharactersForAccountForWorld.characterCount == 0)
        return 0;

    // Next character
    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->res.getCharactersForAccountForWorld.characterCount; req->temp.updateCharacter.i++) {
        DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

        query = "SELECT item_id FROM Items WHERE character_id = ? AND id IN (SELECT item FROM Equipment WHERE id IN (SELECT equip FROM EquippedEquipment))";
        DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u32(&req->res.getCharactersForAccountForWorld.characters[req->temp.updateCharacter.i].id);
        INPUT_BINDER_FINALIZE(req->stmt);

        OUTPUT_BINDER_INIT(1);
        OUTPUT_BINDER_u32(&req->res.getCharactersForAccountForWorld.characters[req->temp.updateCharacter.i].equipment[EQUIP_SLOT_COUNT - 1]);
        OUTPUT_BINDER_FINALIZE(req->stmt);

        req->res.getCharactersForAccountForWorld.characters[req->temp.updateCharacter.i].equipCount = 0;

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
        while (ret != MYSQL_NO_DATA) {
            req->res.getCharactersForAccountForWorld.characters[req->temp.updateCharacter.i].equipment[req->res.getCharactersForAccountForWorld.characters[req->temp.updateCharacter.i].equipCount] = req->res.getCharactersForAccountForWorld.characters[req->temp.updateCharacter.i].equipment[EQUIP_SLOT_COUNT - 1];
            req->res.getCharactersForAccountForWorld.characters[req->temp.updateCharacter.i].equipCount++;

            DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
        }
    }

    END_ASYNC()

    return 0;
}

static int do_get_characters_for_account(struct DatabaseRequest *req, int status)
{
    return 0;
}

static int do_get_characters_exists(struct DatabaseRequest *req, int status)
{
    int ret;
    const char *query = "SELECT EXISTS(SELECT * FROM Characters WHERE name = ?)";
    BEGIN_ASYNC(req)
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_sized_string(req->params.getCharacterExists.nameLength, req->params.getCharacterExists.name);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(1);
    OUTPUT_BINDER_bool(&req->res.getCharacterExists.exists);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    END_ASYNC()

    return 0;
}

static int do_try_create_character(struct DatabaseRequest *req, int status)
{
    int ret;
    my_bool bret;
    const char *query = "INSERT IGNORE INTO Characters (name, account_id, world, job, map, gender, skin, face, hair) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
    BEGIN_ASYNC(req)
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(9);
    INPUT_BINDER_sized_string(req->params.tryCreateCharacter.nameLength, req->params.tryCreateCharacter.name);
    INPUT_BINDER_u32(&req->params.tryCreateCharacter.accountId);
    INPUT_BINDER_u8(&req->params.tryCreateCharacter.world);
    INPUT_BINDER_u16(&req->params.tryCreateCharacter.job);
    INPUT_BINDER_u32(&req->params.tryCreateCharacter.map);
    INPUT_BINDER_u8(&req->params.tryCreateCharacter.gender);
    INPUT_BINDER_u8(&req->params.tryCreateCharacter.skin);
    INPUT_BINDER_u32(&req->params.tryCreateCharacter.face);
    INPUT_BINDER_u32(&req->params.tryCreateCharacter.hair);
    INPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    req->res.tryCreateCharacter.created = mysql_stmt_affected_rows(req->stmt) == 1;
    if (!req->res.tryCreateCharacter.created)
        return 0;
    req->res.tryCreateCharacter.id = mysql_stmt_insert_id(req->stmt);
    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    if (equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT)
        query = "INSERT INTO Items (item_id, character_id) VALUES (?, ?), (?, ?), (?, ?), (?, ?)";
    else
        query = "INSERT INTO Items (item_id, character_id) VALUES (?, ?), (?, ?), (?, ?)";

    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    //case DATABASE_REQUEST_TYPE_TRY_CREATE_CHARACTER:
    INPUT_BINDER_INIT(equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT ? 2 * 4 : 2 * 3);
    // Top
    INPUT_BINDER_u32(&req->params.tryCreateCharacter.top.item.itemId);
    INPUT_BINDER_u32(&req->res.tryCreateCharacter.id);

    // Bottom
    if (equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT) {
        INPUT_BINDER_u32(&req->params.tryCreateCharacter.bottom.item.itemId);
        INPUT_BINDER_u32(&req->res.tryCreateCharacter.id);
    }

    // Shoes
    INPUT_BINDER_u32(&req->params.tryCreateCharacter.shoes.item.itemId);
    INPUT_BINDER_u32(&req->res.tryCreateCharacter.id);

    // Weapon
    INPUT_BINDER_u32(&req->params.tryCreateCharacter.weapon.item.itemId);
    INPUT_BINDER_u32(&req->res.tryCreateCharacter.id);

    INPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    req->temp.id[0] = mysql_stmt_insert_id(req->stmt);
    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    if (equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT)
        query = "INSERT INTO Equipment (item, str, dex, int_, luk, hp, mp, atk, matk, def, mdef, acc, avoid, speed, jump, slots) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
        //query = "INSERT INTO EquippedEquipment (equip) VALUES (?), (?), (?), (?)";
    else
        query = "INSERT INTO Equipment (item, str, dex, int_, luk, hp, mp, atk, matk, def, mdef, acc, avoid, speed, jump, slots) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
        //query = "INSERT INTO EquippedEquipment (equip) VALUES (?), (?), (?)";

    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    req->temp.id[1] = req->temp.id[0] + 1;
    req->temp.id[2] = req->temp.id[0] + 1;
    req->temp.id[3] = req->temp.id[0] + 2;

    INPUT_BINDER_INIT(equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT ? 16 * 4 : 16 * 3);
    // Top
    INPUT_BINDER_u64(&req->temp.id[0]);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.str);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.dex);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.int_);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.luk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.hp);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.mp);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.atk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.matk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.def);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.mdef);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.acc);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.avoid);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.speed);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.top.jump);
    INPUT_BINDER_i8(&req->params.tryCreateCharacter.top.slots);

    // Bottom
    if (equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT) {
        INPUT_BINDER_u64(&req->temp.id[1]);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.str);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.dex);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.int_);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.luk);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.hp);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.mp);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.atk);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.matk);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.def);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.mdef);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.acc);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.avoid);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.speed);
        INPUT_BINDER_i16(&req->params.tryCreateCharacter.bottom.jump);
        INPUT_BINDER_i8(&req->params.tryCreateCharacter.bottom.slots);
        req->temp.id[2]++;
        req->temp.id[3]++;
    }

    // Shoes
    INPUT_BINDER_u64(&req->temp.id[2]);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.str);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.dex);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.int_);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.luk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.hp);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.mp);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.atk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.matk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.def);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.mdef);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.acc);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.avoid);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.speed);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.shoes.jump);
    INPUT_BINDER_i8(&req->params.tryCreateCharacter.shoes.slots);

    // Weapon
    INPUT_BINDER_u64(&req->temp.id[3]);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.str);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.dex);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.int_);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.luk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.hp);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.mp);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.atk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.matk);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.def);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.mdef);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.acc);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.avoid);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.speed);
    INPUT_BINDER_i16(&req->params.tryCreateCharacter.weapon.jump);
    INPUT_BINDER_i8(&req->params.tryCreateCharacter.weapon.slots);

    INPUT_BINDER_FINALIZE(req->stmt);
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    req->temp.id[0] = mysql_stmt_insert_id(req->stmt);
    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    if (equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT)
        query = "INSERT INTO EquippedEquipment (equip) VALUES (?), (?), (?), (?)";
    else
        query = "INSERT INTO EquippedEquipment (equip) VALUES (?), (?), (?)";

    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    req->temp.id[1] = req->temp.id[0] + 1;
    req->temp.id[2] = req->temp.id[0] + 1;
    req->temp.id[3] = req->temp.id[0] + 2;
    INPUT_BINDER_INIT(equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT ? 4 : 3);
    INPUT_BINDER_u64(&req->temp.id[0]);

    if (equip_type_from_id(req->params.tryCreateCharacter.top.item.itemId) == EQUIP_TYPE_COAT) {
        INPUT_BINDER_u64(&req->temp.id[1]);
        req->temp.id[2]++;
        req->temp.id[3]++;
    }

    INPUT_BINDER_u64(&req->temp.id[2]);
    INPUT_BINDER_u64(&req->temp.id[3]);

    INPUT_BINDER_FINALIZE(req->stmt);
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    END_ASYNC()

    return 0;
}

static int do_get_character(struct DatabaseRequest *req, int status)
{
    int ret;
    my_bool bret;
    const char *query;
    BEGIN_ASYNC(req)
    query = "SELECT name, map, spawn, job, level, exp, max_hp, hp, max_mp, mp, str, dex, int_, luk, hpmp, ap, sp, fame, gender, skin, face, hair, mesos, equip_slots, use_slots, setup_slots, etc_slots FROM Characters WHERE id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(27);
    req->res.getCharacter.nameLength = CHARACTER_MAX_NAME_LENGTH;
    OUTPUT_BINDER_sized_string(&req->res.getCharacter.nameLength, req->res.getCharacter.name);
    OUTPUT_BINDER_u32(&req->res.getCharacter.map);
    OUTPUT_BINDER_u8(&req->res.getCharacter.spawnPoint);
    OUTPUT_BINDER_u16(&req->res.getCharacter.job);
    OUTPUT_BINDER_u8(&req->res.getCharacter.level);
    OUTPUT_BINDER_i32(&req->res.getCharacter.exp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.maxHp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.hp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.maxMp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.mp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.str);
    OUTPUT_BINDER_i16(&req->res.getCharacter.dex);
    OUTPUT_BINDER_i16(&req->res.getCharacter.int_);
    OUTPUT_BINDER_i16(&req->res.getCharacter.luk);
    OUTPUT_BINDER_i16(&req->res.getCharacter.hpmp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.ap);
    OUTPUT_BINDER_i16(&req->res.getCharacter.sp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.fame);
    OUTPUT_BINDER_u8(&req->res.getCharacter.gender);
    OUTPUT_BINDER_u8(&req->res.getCharacter.skin);
    OUTPUT_BINDER_u32(&req->res.getCharacter.face);
    OUTPUT_BINDER_u32(&req->res.getCharacter.hair);
    OUTPUT_BINDER_i32(&req->res.getCharacter.mesos);
    OUTPUT_BINDER_u8(&req->res.getCharacter.equipSlots);
    OUTPUT_BINDER_u8(&req->res.getCharacter.useSlots);
    OUTPUT_BINDER_u8(&req->res.getCharacter.setupSlots);
    OUTPUT_BINDER_u8(&req->res.getCharacter.etcSlots);
    OUTPUT_BINDER_FINALIZE(req->stmt);
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "SELECT Equipment.id, Items.id, item_id, flags, owner, level, slots, str, dex, int_, luk, hp, mp, atk, matk, def, mdef, acc, avoid, speed, jump FROM Equipment JOIN Items ON item = Items.id WHERE Equipment.item IN (SELECT id FROM Items WHERE character_id = ?) AND Equipment.id IN (SELECT equip FROM EquippedEquipment)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(21);
    OUTPUT_BINDER_u64(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].id);
    OUTPUT_BINDER_u64(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.id);
    OUTPUT_BINDER_u32(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.itemId);
    OUTPUT_BINDER_u8(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.flags);
    req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.ownerLength = CHARACTER_MAX_NAME_LENGTH;
    OUTPUT_BINDER_nullable_sized_string(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.ownerLength, req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.owner, &req->temp.getCharacter.isNull);
    OUTPUT_BINDER_i8(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].level);
    OUTPUT_BINDER_i8(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].slots);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].str);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].dex);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].int_);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].luk);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].hp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].mp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].atk);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].matk);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].def);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].mdef);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].acc);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].avoid);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].speed);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].jump);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    req->res.getCharacter.equippedCount = 0;
    req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.giverLength = 0;
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    while (ret != MYSQL_NO_DATA) {
        if (req->temp.getCharacter.isNull)
            req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1].item.ownerLength = 0;

        req->res.getCharacter.equippedEquipment[req->res.getCharacter.equippedCount] = req->res.getCharacter.equippedEquipment[EQUIP_SLOT_COUNT - 1];
        req->res.getCharacter.equippedCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "SELECT Equipment.id, Items.id, item_id, flags, owner, level, slots, str, dex, int_, luk, hp, mp, atk, matk, def, mdef, acc, avoid, speed, jump, slot FROM InventoryEquipment JOIN Equipment ON equip = Equipment.id JOIN Items ON item = Items.id WHERE Equipment.item IN (SELECT id FROM Items WHERE character_id = ?) AND Equipment.id IN (SELECT equip FROM InventoryEquipment)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(22);
    OUTPUT_BINDER_u64(&req->res.getCharacter.equipmentInventory[251].equip.id);
    OUTPUT_BINDER_u64(&req->res.getCharacter.equipmentInventory[251].equip.item.id);
    OUTPUT_BINDER_u32(&req->res.getCharacter.equipmentInventory[251].equip.item.itemId);
    OUTPUT_BINDER_u8(&req->res.getCharacter.equipmentInventory[251].equip.item.flags);
    req->res.getCharacter.equipmentInventory[251].equip.item.ownerLength = CHARACTER_MAX_NAME_LENGTH;
    OUTPUT_BINDER_nullable_sized_string(&req->res.getCharacter.equipmentInventory[251].equip.item.ownerLength, req->res.getCharacter.equipmentInventory[251].equip.item.owner, &req->temp.getCharacter.isNull);
    OUTPUT_BINDER_i8(&req->res.getCharacter.equipmentInventory[251].equip.level);
    OUTPUT_BINDER_i8(&req->res.getCharacter.equipmentInventory[251].equip.slots);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.str);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.dex);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.int_);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.luk);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.hp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.mp);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.atk);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.matk);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.def);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.mdef);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.acc);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.avoid);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.speed);
    OUTPUT_BINDER_i16(&req->res.getCharacter.equipmentInventory[251].equip.jump);
    OUTPUT_BINDER_u8(&req->res.getCharacter.equipmentInventory[251].slot);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    req->res.getCharacter.equipCount = 0;
    req->res.getCharacter.equipmentInventory[251].equip.item.giverLength = 0;
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    while (ret != MYSQL_NO_DATA) {
        if (req->temp.getCharacter.isNull)
            req->res.getCharacter.equipmentInventory[251].equip.item.ownerLength = 0;

        req->res.getCharacter.equipmentInventory[req->res.getCharacter.equipCount] = req->res.getCharacter.equipmentInventory[251];
        req->res.getCharacter.equipCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "SELECT id, item_id, flags, owner, slot, count FROM InventoryItems JOIN Items ON item = Items.id WHERE Items.id IN (SELECT id FROM Items WHERE character_id = ?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(6);
    OUTPUT_BINDER_u64(&req->res.getCharacter.inventoryItems[4*252 - 1].item.id);
    OUTPUT_BINDER_u32(&req->res.getCharacter.inventoryItems[4*252 - 1].item.itemId);
    OUTPUT_BINDER_u8(&req->res.getCharacter.inventoryItems[4*252 - 1].item.flags);
    req->res.getCharacter.inventoryItems[4*252 - 1].item.ownerLength = CHARACTER_MAX_NAME_LENGTH;
    OUTPUT_BINDER_nullable_sized_string(&req->res.getCharacter.inventoryItems[4*252 - 1].item.ownerLength, req->res.getCharacter.inventoryItems[4*252 - 1].item.owner, &req->temp.getCharacter.isNull);
    OUTPUT_BINDER_u8(&req->res.getCharacter.inventoryItems[4*252 - 1].slot);
    OUTPUT_BINDER_i16(&req->res.getCharacter.inventoryItems[4*252 - 1].count);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    req->res.getCharacter.itemCount = 0;
    req->res.getCharacter.inventoryItems[252*4 - 1].item.giverLength = 0;
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    while (ret != MYSQL_NO_DATA) {
        if (req->temp.getCharacter.isNull)
            req->res.getCharacter.inventoryItems[252*4 - 1].item.ownerLength = 0;

        req->res.getCharacter.inventoryItems[req->res.getCharacter.itemCount] = req->res.getCharacter.inventoryItems[252*4 - 1];
        req->res.getCharacter.itemCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "SELECT quest_id FROM InProgressQuests WHERE character_id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(1);
    OUTPUT_BINDER_u16(&req->temp.getCharacter.quest);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret != 0, mysql_stmt_store_result, req, status);

    req->res.getCharacter.quests = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(uint16_t));
    if (req->res.getCharacter.quests == NULL)
        return -1;

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    req->res.getCharacter.questCount = 0;
    while (ret != MYSQL_NO_DATA) {
        req->res.getCharacter.quests[req->res.getCharacter.questCount] = req->temp.getCharacter.quest;
        req->res.getCharacter.questCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "SELECT quest_id, progress_id, progress FROM Progresses WHERE character_id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(3);
    OUTPUT_BINDER_u16(&req->temp.getCharacter.quest);
    OUTPUT_BINDER_u32(&req->temp.getCharacter.progressId);
    OUTPUT_BINDER_i16(&req->temp.getCharacter.progress);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret != 0, mysql_stmt_store_result, req, status);

    req->res.getCharacter.progresses = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(struct DatabaseProgress));
    if (req->res.getCharacter.progresses == NULL)
        return -1;

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    req->res.getCharacter.progressCount = 0;
    while (ret != MYSQL_NO_DATA) {
        req->res.getCharacter.progresses[req->res.getCharacter.progressCount].questId = req->temp.getCharacter.quest;
        req->res.getCharacter.progresses[req->res.getCharacter.progressCount].progressId = req->temp.getCharacter.progressId;
        req->res.getCharacter.progresses[req->res.getCharacter.progressCount].progress = req->temp.getCharacter.progress;
        req->res.getCharacter.progressCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "SELECT quest_id, time FROM CompletedQuests WHERE character_id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(2);
    OUTPUT_BINDER_u16(&req->temp.getCharacter.quest);
    OUTPUT_BINDER_time(&req->temp.getCharacter.time);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret != 0, mysql_stmt_store_result, req, status);

    req->res.getCharacter.completedQuests = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(struct DatabaseCompletedQuest));
    if (req->res.getCharacter.completedQuests == NULL)
        return -1;

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    req->res.getCharacter.completedQuestCount = 0;
    while (ret != MYSQL_NO_DATA) {
        req->res.getCharacter.completedQuests[req->res.getCharacter.completedQuestCount].id = req->temp.getCharacter.quest;
        req->res.getCharacter.completedQuests[req->res.getCharacter.completedQuestCount].time = req->temp.getCharacter.time;
        req->res.getCharacter.completedQuestCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "SELECT skill_id, level, master FROM Skills WHERE character_id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.getCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    OUTPUT_BINDER_INIT(3);
    OUTPUT_BINDER_u32(&req->temp.getCharacter.skillId);
    OUTPUT_BINDER_i8(&req->temp.getCharacter.skillLevel);
    OUTPUT_BINDER_i8(&req->temp.getCharacter.skillMasterLevel);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(ret, ret != 0, mysql_stmt_store_result, req, status);

    req->res.getCharacter.skills = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(struct DatabaseSkill));
    if (req->res.getCharacter.skills == NULL)
        return -1;

    DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    req->res.getCharacter.skillCount = 0;
    while (ret != MYSQL_NO_DATA) {
        req->res.getCharacter.skills[req->res.getCharacter.skillCount].id = req->temp.getCharacter.skillId;
        req->res.getCharacter.skills[req->res.getCharacter.skillCount].level = req->temp.getCharacter.skillLevel;
        req->res.getCharacter.skills[req->res.getCharacter.skillCount].masterLevel = req->temp.getCharacter.skillMasterLevel;
        req->res.getCharacter.skillCount++;

        DO_ASYNC(ret, ret == 1, mysql_stmt_fetch, req, status);
    }

    END_ASYNC()

    return 0;
}

static int do_update_character(struct DatabaseRequest *req, int status)
{
    int ret;
    my_bool bret;
    const char *query;
    BEGIN_ASYNC(req)
    query = "UPDATE Characters SET map = ?, spawn = ?, job = ?, level = ?, exp = ?, max_hp = ?, hp = ?, max_mp = ?, mp = ?, str = ?, dex = ?, int_ = ?, luk = ?, ap = ?, sp = ?, fame = ?, mesos = ?, skin = ?, face = ?, hair = ?, equip_slots = ?, use_slots = ?, setup_slots = ?, etc_slots = ? WHERE id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    INPUT_BINDER_INIT(25);
    INPUT_BINDER_u32(&req->params.updateCharacter.map);
    INPUT_BINDER_u8(&req->params.updateCharacter.spawnPoint);
    INPUT_BINDER_u16(&req->params.updateCharacter.job);
    INPUT_BINDER_u8(&req->params.updateCharacter.level);
    INPUT_BINDER_i32(&req->params.updateCharacter.exp);
    INPUT_BINDER_i16(&req->params.updateCharacter.maxHp);
    INPUT_BINDER_i16(&req->params.updateCharacter.hp);
    INPUT_BINDER_i16(&req->params.updateCharacter.maxMp);
    INPUT_BINDER_i16(&req->params.updateCharacter.mp);
    INPUT_BINDER_i16(&req->params.updateCharacter.str);
    INPUT_BINDER_i16(&req->params.updateCharacter.dex);
    INPUT_BINDER_i16(&req->params.updateCharacter.int_);
    INPUT_BINDER_i16(&req->params.updateCharacter.luk);
    INPUT_BINDER_i16(&req->params.updateCharacter.ap);
    INPUT_BINDER_i16(&req->params.updateCharacter.sp);
    INPUT_BINDER_i16(&req->params.updateCharacter.fame);
    INPUT_BINDER_i32(&req->params.updateCharacter.mesos);
    INPUT_BINDER_u8(&req->params.updateCharacter.skin);
    INPUT_BINDER_u32(&req->params.updateCharacter.face);
    INPUT_BINDER_u32(&req->params.updateCharacter.hair);
    INPUT_BINDER_u8(&req->params.updateCharacter.equipSlots);
    INPUT_BINDER_u8(&req->params.updateCharacter.useSlots);
    INPUT_BINDER_u8(&req->params.updateCharacter.setupSlots);
    INPUT_BINDER_u8(&req->params.updateCharacter.etcSlots);
    INPUT_BINDER_u32(&req->params.updateCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "UPDATE Items SET deleted = 1 WHERE character_id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));
    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.updateCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    // Update existing items
    // TODO: Don't run this when the item count is 0
    query = "INSERT INTO Items (id, character_id, item_id, flags, owner, giver) VALUES (?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE character_id = ?, flags = ?, owner = ?, giver = ?, deleted = 0";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    struct {
        uint64_t id;
        uint32_t charId;
        uint32_t itemId;
        uint8_t flags;
        char owner[CHARACTER_MAX_NAME_LENGTH];
        unsigned long ownerLength;
        char owner_ind;
        char giver[CHARACTER_MAX_NAME_LENGTH];
        unsigned long giverLength;
        char giver_ind;
    } *data = malloc((req->params.updateCharacter.equippedCount + req->params.updateCharacter.equipCount + req->params.updateCharacter.itemCount) * sizeof(*data));
    req->temp.updateCharacter.data = data;
    if (data == NULL)
        return -1;

    size_t count = 0;
    for (size_t i = 0; i < 3; i++) {
        size_t item_count = i == 0 ?
            req->params.updateCharacter.equippedCount : (i == 1 ?
                    req->params.updateCharacter.equipCount :
                    req->params.updateCharacter.itemCount);

        for (size_t j = 0; j < item_count; j++) {
            struct DatabaseItem *item = i == 0 ?
                &req->params.updateCharacter.equippedEquipment[j].item : (i == 1 ?
                        &req->params.updateCharacter.equipmentInventory[j].equip.item :
                        &req->params.updateCharacter.inventoryItems[j].item);

            if (item->id != 0) {
                data[count].id = item->id;
                data[count].charId = req->params.updateCharacter.id;
                data[count].itemId = item->itemId;
                data[count].flags = item->flags;
                strncpy(data[count].owner, item->owner, item->ownerLength);
                data[count].ownerLength = item->ownerLength;
                data[count].owner_ind = data[count].ownerLength != 0 ? STMT_INDICATOR_NONE : STMT_INDICATOR_NULL;
                strncpy(data[count].giver, item->giver, item->giverLength);
                data[count].giverLength = item->giverLength;
                data[count].giver_ind = data[count].giverLength != 0 ? STMT_INDICATOR_NONE : STMT_INDICATOR_NULL;
                count++;
            }
        }
    }

    INPUT_BINDER_INIT(10);
    INPUT_BINDER_u64(&data->id);
    INPUT_BINDER_u32(&data->charId);
    INPUT_BINDER_u32(&data->itemId);
    INPUT_BINDER_u8(&data->flags);
    INPUT_BINDER_bulk_sized_string(data->owner, &data->ownerLength, &data->owner_ind);
    INPUT_BINDER_bulk_sized_string(data->giver, &data->giverLength, &data->giver_ind);
    INPUT_BINDER_u32(&data->charId);
    INPUT_BINDER_u8(&data->flags);
    INPUT_BINDER_bulk_sized_string(data->owner, &data->ownerLength, &data->owner_ind);
    INPUT_BINDER_bulk_sized_string(data->giver, &data->giverLength, &data->giver_ind);
    INPUT_BINDER_FINALIZE(req->stmt);

    mysql_stmt_attr_set(req->stmt, STMT_ATTR_ROW_SIZE, (size_t[]) { sizeof(*data) });

    mysql_stmt_attr_set(req->stmt, STMT_ATTR_ARRAY_SIZE, (unsigned int[]) { count });

    // TODO: Find a way to free up the data when DO_ASYNC fails
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    free(req->temp.updateCharacter.data);

    mysql_stmt_attr_set(req->stmt, STMT_ATTR_ROW_SIZE, (size_t[]) { 0 });

    mysql_stmt_attr_set(req->stmt, STMT_ATTR_ARRAY_SIZE, (unsigned int[]) { 0 });

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    // Insert new items - Probably can't batch the request as we need the ID of each inserted item
    query = "INSERT INTO Items (character_id, item_id, flags, owner, giver) VALUES (?, ?, ?, ?, ?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    // Insert new equipped items
    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equippedCount; req->temp.updateCharacter.i++) {
        struct DatabaseItem *item = &req->params.updateCharacter.equippedEquipment[req->temp.updateCharacter.i].item;
        if (item->id == 0) {
            INPUT_BINDER_INIT(5);
            INPUT_BINDER_u32(&req->params.updateCharacter.id);
            INPUT_BINDER_u32(&item->itemId);
            INPUT_BINDER_u8(&item->flags);
            if (item->ownerLength == 0)
                INPUT_BINDER_null();
            else
                INPUT_BINDER_sized_string(item->ownerLength, item->owner);

            if (item->giverLength == 0)
                INPUT_BINDER_null();
            else
                INPUT_BINDER_sized_string(item->giverLength, item->giver);
            INPUT_BINDER_FINALIZE(req->stmt);

            DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
            item = &req->params.updateCharacter.equippedEquipment[req->temp.updateCharacter.i].item;
            item->id = mysql_stmt_insert_id(req->stmt);
        }
    }

    // Insert new equipment-inventory items
    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equipCount; req->temp.updateCharacter.i++) {
        struct DatabaseItem *item = &req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].equip.item;
        if (item->id == 0) {
            INPUT_BINDER_INIT(5);
            INPUT_BINDER_u32(&req->params.updateCharacter.id);
            INPUT_BINDER_u32(&item->itemId);
            INPUT_BINDER_u8(&item->flags);
            if (item->ownerLength == 0)
                INPUT_BINDER_null();
            else
                INPUT_BINDER_sized_string(item->ownerLength, item->owner);

            if (item->giverLength == 0)
                INPUT_BINDER_null();
            else
                INPUT_BINDER_sized_string(item->giverLength, item->giver);
            INPUT_BINDER_FINALIZE(req->stmt);

            DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
            item = &req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].equip.item;
            item->id = mysql_stmt_insert_id(req->stmt);
        }
    }

    // Insert new inventory items
    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.itemCount; req->temp.updateCharacter.i++) {
        struct DatabaseItem *item = &req->params.updateCharacter.inventoryItems[req->temp.updateCharacter.i].item;
        if (item->id == 0) {
            INPUT_BINDER_INIT(5);
            INPUT_BINDER_u32(&req->params.updateCharacter.id);
            INPUT_BINDER_u32(&item->itemId);
            INPUT_BINDER_u8(&item->flags);
            if (item->ownerLength == 0)
                INPUT_BINDER_null();
            else
                INPUT_BINDER_sized_string(item->ownerLength, item->owner);

            if (item->giverLength == 0)
                INPUT_BINDER_null();
            else
                INPUT_BINDER_sized_string(item->giverLength, item->giver);
            INPUT_BINDER_FINALIZE(req->stmt);

            DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
            item = &req->params.updateCharacter.inventoryItems[req->temp.updateCharacter.i].item;
            item->id = mysql_stmt_insert_id(req->stmt);
        }
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    // Update existing equipment
    // TODO: Don't run this when the item count is 0
    query = "INSERT INTO Equipment VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE level = ?, slots = ?, str = ?, dex = ?, int_ = ?, luk = ?, hp = ?, mp = ?, atk = ?, matk = ?, def = ?, mdef = ?, acc = ?, avoid = ?, speed = ?, jump = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    {
        struct {
            uint64_t id;
            uint64_t item;
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
            int16_t speed;
            int16_t jump;
        } *data = malloc((req->params.updateCharacter.equippedCount + req->params.updateCharacter.equipCount) * sizeof(*data));
        req->temp.updateCharacter.data = data;

        size_t count = 0;
        for (size_t i = 0; i < 2; i++) {
            size_t item_count = i == 0 ?
                req->params.updateCharacter.equippedCount :
                req->params.updateCharacter.equipCount;

            for (size_t j = 0; j < item_count; j++) {
                struct DatabaseEquipment *equip = i == 0 ?
                    &req->params.updateCharacter.equippedEquipment[j] :
                    &req->params.updateCharacter.equipmentInventory[j].equip;

                if (equip->id != 0) {
                    data[count].id = equip->id;
                    data[count].item = equip->item.id;
                    data[count].level = equip->level;
                    data[count].slots = equip->slots;
                    data[count].str = equip->str;
                    data[count].dex = equip->dex;
                    data[count].int_ = equip->int_;
                    data[count].luk = equip->luk;
                    data[count].hp = equip->hp;
                    data[count].mp = equip->mp;
                    data[count].atk = equip->atk;
                    data[count].matk = equip->matk;
                    data[count].def = equip->def;
                    data[count].mdef = equip->mdef;
                    data[count].acc = equip->acc;
                    data[count].avoid = equip->avoid;
                    data[count].speed = equip->speed;
                    data[count].jump = equip->jump;
                    count++;
                }
            }
        }

        INPUT_BINDER_INIT(34);
        INPUT_BINDER_u64(&data->id);
        INPUT_BINDER_u64(&data->item);
        INPUT_BINDER_i8(&data->level);
        INPUT_BINDER_i8(&data->slots);
        INPUT_BINDER_i16(&data->str);
        INPUT_BINDER_i16(&data->dex);
        INPUT_BINDER_i16(&data->int_);
        INPUT_BINDER_i16(&data->luk);
        INPUT_BINDER_i16(&data->hp);
        INPUT_BINDER_i16(&data->mp);
        INPUT_BINDER_i16(&data->atk);
        INPUT_BINDER_i16(&data->matk);
        INPUT_BINDER_i16(&data->def);
        INPUT_BINDER_i16(&data->mdef);
        INPUT_BINDER_i16(&data->acc);
        INPUT_BINDER_i16(&data->avoid);
        INPUT_BINDER_i16(&data->speed);
        INPUT_BINDER_i16(&data->jump);

        // UPDATE
        INPUT_BINDER_i8(&data->level);
        INPUT_BINDER_i8(&data->slots);
        INPUT_BINDER_i16(&data->str);
        INPUT_BINDER_i16(&data->dex);
        INPUT_BINDER_i16(&data->int_);
        INPUT_BINDER_i16(&data->luk);
        INPUT_BINDER_i16(&data->hp);
        INPUT_BINDER_i16(&data->mp);
        INPUT_BINDER_i16(&data->atk);
        INPUT_BINDER_i16(&data->matk);
        INPUT_BINDER_i16(&data->def);
        INPUT_BINDER_i16(&data->mdef);
        INPUT_BINDER_i16(&data->acc);
        INPUT_BINDER_i16(&data->avoid);
        INPUT_BINDER_i16(&data->speed);
        INPUT_BINDER_i16(&data->jump);
        INPUT_BINDER_FINALIZE(req->stmt);

        mysql_stmt_attr_set(req->stmt, STMT_ATTR_ROW_SIZE, (size_t[]) { sizeof(*data) });

        mysql_stmt_attr_set(req->stmt, STMT_ATTR_ARRAY_SIZE, (unsigned int[]) { count });

        // TODO: Find a way to free up the data when DO_ASYNC fails
        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
        free(req->temp.updateCharacter.data);

        mysql_stmt_attr_set(req->stmt, STMT_ATTR_ROW_SIZE, (size_t[]) { 0 });

        mysql_stmt_attr_set(req->stmt, STMT_ATTR_ARRAY_SIZE, (unsigned int[]) { 0 });

        DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);
    }

    // Insert new equipment - Probably can't batch the request as we need the ID of each inserted equipment
    query = "INSERT INTO Equipment (item, level, slots, str, dex, int_, luk, hp, mp, atk, matk, def, mdef, acc, avoid, speed, jump) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    // Insert new equipped equipment
    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equippedCount; req->temp.updateCharacter.i++) {
        struct DatabaseEquipment *equip = &req->params.updateCharacter.equippedEquipment[req->temp.updateCharacter.i];
        if (equip->id == 0) {
            INPUT_BINDER_INIT(17);
            INPUT_BINDER_u64(&equip->item.id);
            INPUT_BINDER_i8(&equip->level);
            INPUT_BINDER_i8(&equip->slots);
            INPUT_BINDER_i16(&equip->str);
            INPUT_BINDER_i16(&equip->dex);
            INPUT_BINDER_i16(&equip->int_);
            INPUT_BINDER_i16(&equip->luk);
            INPUT_BINDER_i16(&equip->hp);
            INPUT_BINDER_i16(&equip->mp);
            INPUT_BINDER_i16(&equip->atk);
            INPUT_BINDER_i16(&equip->matk);
            INPUT_BINDER_i16(&equip->def);
            INPUT_BINDER_i16(&equip->mdef);
            INPUT_BINDER_i16(&equip->acc);
            INPUT_BINDER_i16(&equip->avoid);
            INPUT_BINDER_i16(&equip->speed);
            INPUT_BINDER_i16(&equip->jump);
            INPUT_BINDER_FINALIZE(req->stmt);

            DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
            equip = &req->params.updateCharacter.equippedEquipment[req->temp.updateCharacter.i];
            equip->id = mysql_stmt_insert_id(req->stmt);
        }
    }

    // Insert new equipment-inventory equipment
    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equipCount; req->temp.updateCharacter.i++) {
        struct DatabaseEquipment *equip = &req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].equip;
        if (equip->id == 0) {
            INPUT_BINDER_INIT(17);
            INPUT_BINDER_u64(&equip->item.id);
            INPUT_BINDER_i8(&equip->level);
            INPUT_BINDER_i8(&equip->slots);
            INPUT_BINDER_i16(&equip->str);
            INPUT_BINDER_i16(&equip->dex);
            INPUT_BINDER_i16(&equip->int_);
            INPUT_BINDER_i16(&equip->luk);
            INPUT_BINDER_i16(&equip->hp);
            INPUT_BINDER_i16(&equip->mp);
            INPUT_BINDER_i16(&equip->atk);
            INPUT_BINDER_i16(&equip->matk);
            INPUT_BINDER_i16(&equip->def);
            INPUT_BINDER_i16(&equip->mdef);
            INPUT_BINDER_i16(&equip->acc);
            INPUT_BINDER_i16(&equip->avoid);
            INPUT_BINDER_i16(&equip->speed);
            INPUT_BINDER_i16(&equip->jump);
            INPUT_BINDER_FINALIZE(req->stmt);

            DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
            equip = &req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].equip;
            equip->id = mysql_stmt_insert_id(req->stmt);
        }
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "DELETE FROM EquippedEquipment WHERE equip = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equippedCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u64(&req->params.updateCharacter.equippedEquipment[req->temp.updateCharacter.i].id);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equipCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u64(&req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].equip.id);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "DELETE FROM InventoryEquipment WHERE equip = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equippedCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u64(&req->params.updateCharacter.equippedEquipment[req->temp.updateCharacter.i].id);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equipCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u64(&req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].equip.id);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "INSERT INTO EquippedEquipment VALUES (?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equippedCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u64(&req->params.updateCharacter.equippedEquipment[req->temp.updateCharacter.i].id);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "INSERT INTO InventoryEquipment VALUES (?, ?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.equipCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(2);
        INPUT_BINDER_u64(&req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].equip.id);
        INPUT_BINDER_u8(&req->params.updateCharacter.equipmentInventory[req->temp.updateCharacter.i].slot);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "INSERT INTO InventoryItems VALUES (?, ?, ?) ON DUPLICATE KEY UPDATE slot = ?, count = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.itemCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(5);
        INPUT_BINDER_u64(&req->params.updateCharacter.inventoryItems[req->temp.updateCharacter.i].item.id);
        INPUT_BINDER_u8(&req->params.updateCharacter.inventoryItems[req->temp.updateCharacter.i].slot);
        INPUT_BINDER_i16(&req->params.updateCharacter.inventoryItems[req->temp.updateCharacter.i].count);
        INPUT_BINDER_u8(&req->params.updateCharacter.inventoryItems[req->temp.updateCharacter.i].slot);
        INPUT_BINDER_i16(&req->params.updateCharacter.inventoryItems[req->temp.updateCharacter.i].count);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "DELETE FROM Items WHERE deleted = 1";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));
    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    // TODO: Maybe use a soft-delete
    query = "DELETE FROM InProgressQuests WHERE character_id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));
    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.updateCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "DELETE FROM CompletedQuests WHERE character_id = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));
    INPUT_BINDER_INIT(1);
    INPUT_BINDER_u32(&req->params.updateCharacter.id);
    INPUT_BINDER_FINALIZE(req->stmt);

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "INSERT INTO InProgressQuests (character_id, quest_id) VALUES (?, ?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.questCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(2);
        INPUT_BINDER_u32(&req->params.updateCharacter.id);
        INPUT_BINDER_u16(&req->params.updateCharacter.quests[req->temp.updateCharacter.i]);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "INSERT INTO Progresses (character_id, quest_id, progress_id, progress) VALUES (?, ?, ?, ?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.progressCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(4);
        INPUT_BINDER_u32(&req->params.updateCharacter.id);
        INPUT_BINDER_u16(&req->params.updateCharacter.progresses[req->temp.updateCharacter.i].questId);
        INPUT_BINDER_u32(&req->params.updateCharacter.progresses[req->temp.updateCharacter.i].progressId);
        INPUT_BINDER_i16(&req->params.updateCharacter.progresses[req->temp.updateCharacter.i].progress);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    query = "INSERT INTO CompletedQuests (character_id, quest_id, time) VALUES (?, ?, ?)";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    for (req->temp.updateCharacter.i = 0; req->temp.updateCharacter.i < req->params.updateCharacter.completedQuestCount; req->temp.updateCharacter.i++) {
        INPUT_BINDER_INIT(3);
        INPUT_BINDER_u32(&req->params.updateCharacter.id);
        INPUT_BINDER_u16(&req->params.updateCharacter.completedQuests[req->temp.updateCharacter.i].id);
        INPUT_BINDER_time(&req->params.updateCharacter.completedQuests[req->temp.updateCharacter.i].time);
        INPUT_BINDER_FINALIZE(req->stmt);

        DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    }

    query = "INSERT INTO Skills VALUES (?, ?, ?, ?) ON DUPLICATE KEY UPDATE level = ?, master = ?";
    DO_ASYNC(ret, ret != 0, mysql_stmt_prepare, req, status, query, strlen(query));

    {
        struct {
            uint32_t char_id;
            uint32_t skill_id;
            int8_t level;
            int8_t master_level;
        } *data = malloc((req->params.updateCharacter.skillCount) * sizeof(*data));
        req->temp.updateCharacter.data = data;
        if (data == NULL)
            return -1;

        for (size_t i = 0; i < req->params.updateCharacter.skillCount; i++) {
            data[i].char_id = req->params.updateCharacter.id;
            data[i].skill_id = req->params.updateCharacter.skills[i].id;
            data[i].level = req->params.updateCharacter.skills[i].level;
            data[i].master_level = req->params.updateCharacter.skills[i].masterLevel;
        }

        INPUT_BINDER_INIT(6);
        INPUT_BINDER_u32(&data->char_id);
        INPUT_BINDER_u32(&data->skill_id);
        INPUT_BINDER_i8(&data->level);
        INPUT_BINDER_i8(&data->master_level);

        // UPDATE
        INPUT_BINDER_i8(&data->level);
        INPUT_BINDER_i8(&data->master_level);
        INPUT_BINDER_FINALIZE(req->stmt);

        mysql_stmt_attr_set(req->stmt, STMT_ATTR_ROW_SIZE, (size_t[]) { sizeof(*data) });

        mysql_stmt_attr_set(req->stmt, STMT_ATTR_ARRAY_SIZE, (unsigned int[]) { req->params.updateCharacter.skillCount });

    }

    DO_ASYNC(ret, ret != 0, mysql_stmt_execute, req, status);
    free(req->temp.updateCharacter.data);

    //mysql_stmt_attr_set(req->stmt, STMT_ATTR_ROW_SIZE, (size_t[]) { 0 });

    //mysql_stmt_attr_set(req->stmt, STMT_ATTR_ARRAY_SIZE, (unsigned int[]) { 0 });

    //DO_ASYNC(bret, bret, mysql_stmt_reset, req, status);

    END_ASYNC();

    return 0;
}

static int do_get_monster_drops(struct DatabaseRequest *req, int status)
{
    int ret;
    const char *query = "SELECT DISTINCT monster_id FROM MonsterItemDrops UNION SELECT DISTINCT monster_id FROM MonsterQuestItemDrops UNION SELECT DISTINCT monster_id FROM MonsterMesoDrops UNION SELECT DISTINCT monster_id FROM MonsterMultiItemDrops";
    assert(mysql_stmt_prepare(req->stmt, query, strlen(query)) == 0);

    uint32_t id;
    OUTPUT_BINDER_INIT(1);
    OUTPUT_BINDER_u32(&id);
    OUTPUT_BINDER_FINALIZE(req->stmt);

    assert(mysql_stmt_execute(req->stmt) == 0);
    assert(mysql_stmt_store_result(req->stmt) == 0);

    req->res.getMonsterDrops.monsters = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(struct MonsterDrops));
    if (req->res.getMonsterDrops.monsters == NULL)
        return -1;

    ret = mysql_stmt_fetch(req->stmt);
    assert(ret == 0 || ret == MYSQL_NO_DATA);
    while (ret != MYSQL_NO_DATA) {
        req->res.getMonsterDrops.monsters[req->res.getMonsterDrops.count].id = id;
        req->res.getMonsterDrops.count++;

        ret = mysql_stmt_fetch(req->stmt);
        assert(ret == 0 || ret == MYSQL_NO_DATA);
    }

    for (size_t i = 0; i < req->res.getMonsterDrops.count; i++) {
        struct MonsterDrops *monster = &req->res.getMonsterDrops.monsters[i];
        assert(mysql_stmt_reset(req->stmt) == 0);

        query = "SELECT item_id, chance FROM MonsterItemDrops WHERE monster_id = ?";
        mysql_stmt_prepare(req->stmt, query, strlen(query));

        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u32(&monster->id);
        INPUT_BINDER_FINALIZE(req->stmt);

        OUTPUT_BINDER_INIT(2);
        OUTPUT_BINDER_u32(&req->temp.drop.itemId);
        OUTPUT_BINDER_i32(&req->temp.drop.chance);
        OUTPUT_BINDER_FINALIZE(req->stmt);

        assert(mysql_stmt_execute(req->stmt) == 0);
        assert(mysql_stmt_store_result(req->stmt) == 0);

        monster->itemDrops.drops = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(struct MonsterItemDrops));

        monster->itemDrops.count = 0;
        ret = mysql_stmt_fetch(req->stmt);
        assert(ret == 0 || ret == MYSQL_NO_DATA);
        while (ret != MYSQL_NO_DATA) {
            monster->itemDrops.drops[monster->itemDrops.count].itemId = req->temp.drop.itemId;
            monster->itemDrops.drops[monster->itemDrops.count].chance = req->temp.drop.chance;
            monster->itemDrops.count++;

            ret = mysql_stmt_fetch(req->stmt);
            assert(ret == 0 || ret == MYSQL_NO_DATA);
        }

        assert(mysql_stmt_reset(req->stmt) == 0);

        query = "SELECT item_id, quest_id, chance FROM MonsterQuestItemDrops WHERE monster_id = ?";
        assert(mysql_stmt_prepare(req->stmt, query, strlen(query)) == 0);

        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u32(&monster->id);
        INPUT_BINDER_FINALIZE(req->stmt);

        OUTPUT_BINDER_INIT(3);
        OUTPUT_BINDER_u32(&req->temp.drop.itemId);
        OUTPUT_BINDER_u16(&req->temp.drop.questId);
        OUTPUT_BINDER_i32(&req->temp.drop.chance);
        OUTPUT_BINDER_FINALIZE(req->stmt);

        assert(mysql_stmt_execute(req->stmt) == 0);
        assert(mysql_stmt_store_result(req->stmt) == 0);

        monster->questItemDrops.drops = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(struct MonsterItemDrops));

        monster->questItemDrops.count = 0;
        ret = mysql_stmt_fetch(req->stmt);
        assert(ret == 0 || ret == MYSQL_NO_DATA);
        while (ret != MYSQL_NO_DATA) {
            monster->questItemDrops.drops[monster->questItemDrops.count].itemId = req->temp.drop.itemId;
            monster->questItemDrops.drops[monster->questItemDrops.count].questId = req->temp.drop.questId;
            monster->questItemDrops.drops[monster->questItemDrops.count].chance = req->temp.drop.chance;
            monster->questItemDrops.count++;

            ret = mysql_stmt_fetch(req->stmt);
            assert(ret == 0 || ret == MYSQL_NO_DATA);
        }

        assert(mysql_stmt_reset(req->stmt) == 0);

        query = "SELECT min, max, chance FROM MonsterMesoDrops WHERE monster_id = ?";
        assert(mysql_stmt_prepare(req->stmt, query, strlen(query)) == 0);

        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u32(&monster->id);
        INPUT_BINDER_FINALIZE(req->stmt);

        OUTPUT_BINDER_INIT(3);
        OUTPUT_BINDER_i32(&req->temp.drop.min);
        OUTPUT_BINDER_i32(&req->temp.drop.max);
        OUTPUT_BINDER_i32(&req->temp.drop.chance);
        OUTPUT_BINDER_FINALIZE(req->stmt);

        assert(mysql_stmt_execute(req->stmt) == 0);
        ret = mysql_stmt_fetch(req->stmt);
        assert(ret == 0 || ret == MYSQL_NO_DATA);
        if (ret != MYSQL_NO_DATA) {
            monster->mesoDrop.min = req->temp.drop.min;
            monster->mesoDrop.max = req->temp.drop.max;
            monster->mesoDrop.chance = req->temp.drop.chance;
        } else {
            monster->mesoDrop.max = 0;
        }

        assert(mysql_stmt_reset(req->stmt) == 0);

        query = "SELECT item_id, min, max, chance FROM MonsterMultiItemDrops WHERE monster_id = ?";
        assert(mysql_stmt_prepare(req->stmt, query, strlen(query)) == 0);

        INPUT_BINDER_INIT(1);
        INPUT_BINDER_u32(&monster->id);
        INPUT_BINDER_FINALIZE(req->stmt);

        OUTPUT_BINDER_INIT(4);
        OUTPUT_BINDER_u32(&req->temp.drop.itemId);
        OUTPUT_BINDER_i32(&req->temp.drop.min);
        OUTPUT_BINDER_i32(&req->temp.drop.max);
        OUTPUT_BINDER_i32(&req->temp.drop.chance);
        OUTPUT_BINDER_FINALIZE(req->stmt);
        assert(mysql_stmt_execute(req->stmt) == 0);
        assert(mysql_stmt_store_result(req->stmt) == 0);

        monster->multiItemDrops.drops = malloc(mysql_stmt_num_rows(req->stmt) * sizeof(struct MultiItemDrop));;

        monster->multiItemDrops.count = 0;
        ret = mysql_stmt_fetch(req->stmt);
        assert(ret == 0 || ret == MYSQL_NO_DATA);
        while (ret != MYSQL_NO_DATA) {
            monster->multiItemDrops.drops[monster->multiItemDrops.count].id = req->temp.drop.itemId;
            monster->multiItemDrops.drops[monster->multiItemDrops.count].min = req->temp.drop.min;
            monster->multiItemDrops.drops[monster->multiItemDrops.count].max = req->temp.drop.max;
            monster->multiItemDrops.drops[monster->multiItemDrops.count].chance = req->temp.drop.chance;
            monster->multiItemDrops.count++;

            ret = mysql_stmt_fetch(req->stmt);
            assert(ret == 0 || ret == MYSQL_NO_DATA);
        }
    }

    return 0;
}

static void lock_queue_init(struct LockQueue *queue)
{
    queue->head = NULL;
}

static int lock_queue_enqueue(struct LockQueue *queue, int value)
{
    struct LockQueueNode *new = malloc(sizeof(struct LockQueueNode));
    if (new == NULL)
        return -1;

    new->next = NULL;
    new->value = value;

    queue->last = (queue->head == NULL) ? (queue->head = new) : (queue->last->next = new);
    return 0;
}

static int lock_queue_dequeue(struct LockQueue *queue)
{
    struct LockQueueNode *next = queue->head->next;
    free(queue->head);
    queue->head = next;
    return queue->head == NULL ? -1 : queue->head->value;
}

static bool lock_queue_empty(struct LockQueue *queue)
{
    return queue->head == NULL;
}

static int mariadb_to_poll(int status)
{
    int ret = 0;
    if (status & MYSQL_WAIT_READ)
        ret |= POLLIN;
    if (status & MYSQL_WAIT_WRITE)
        ret |= POLLOUT;
    if (status & MYSQL_WAIT_EXCEPT)
        ret |= POLLPRI;

    return ret;
}

static int poll_to_mariadb(int status)
{
    int ret = 0;
    if (status & POLLIN)
        ret |= MYSQL_WAIT_READ;
    if (status & POLLPRI)
        ret |= MYSQL_WAIT_EXCEPT;
    if (status & POLLOUT)
        ret |= MYSQL_WAIT_WRITE;

    return ret;
}

