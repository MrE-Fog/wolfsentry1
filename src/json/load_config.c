/*
 * json/load_config.c
 *
 * Copyright (C) 2021-2023 wolfSSL Inc.
 *
 * This file is part of wolfSentry.
 *
 * wolfSentry is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSentry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

#include "wolfsentry/wolfsentry_json.h"
#include "wolfsentry/wolfsentry_util.h"

#define WOLFSENTRY_SOURCE_ID WOLFSENTRY_SOURCE_ID_JSON_LOAD_CONFIG_C

#include <stdlib.h>

#define MAX_IPV4_ADDR_BITS (sizeof(struct in_addr) * BITS_PER_BYTE)
#define MAX_IPV6_ADDR_BITS (sizeof(struct in6_addr) * BITS_PER_BYTE)
#define MAX_MAC_ADDR_BITS 64

#ifdef WOLFSENTRY_LWIP
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#ifndef WOLFSENTRY_NO_GETPROTOBY
#include <netdb.h>
#endif

/* note that the order of objects in the JSON is semantically significant.
 * Thus, any application-level JSON preprocessing must be ES5-compliant
 * (preserve the order of pairs exactly).
 */

/*

{

"wolfsentry-config-version" : 1,

"config-update" : {
    "max-connection-count" : number,
    "penaltybox-duration" : number|string, // allow suffixes s,m,h,d
    "derog-thresh-for-penalty-boxing" : number,
    "derog-thresh-ignore-commendable" : true|false,
    "commendable-clears-derogatory" : true|false,
},

"events-insert" : [
{
    "priority" : number,
    "label" : string,
    "config" : {
        "max-connection-count" : number
        "penalty-box-duration" : number|string // allow suffixes s,m,h,d
        "derog-thresh-for-penalty-boxing" : number,
        "derog-thresh-ignore-commendable" : true|false,
        "commendable-clears-derogatory" : true|false,
    }
    "actions" : [ string ... ],
    "insert-event" : string,
    "match-event" : string,
    "update-event" : string,
    "delete-event" : string
    "decision-event" : string

}
],

"default-policies" : {
    "default-policy" : "accept" | "reject",
    "default-event" : string
},

"static-routes-insert" : [
{
    "parent-event" : string,
    "tcplike-port-numbers" : true|false,
    "direction-in" : true|false,
    "direction-out" : true|false,
    "penalty-boxed" : true|false,
    "green-listed" : true|false,
    "dont-count-hits" : true|false,
    "dont-count-current-connections" : true|false,
    "family" : string|number,
    "protocol" : string|number,
    "remote" : {
        "port" : string|number,
        "address" : string,
        "prefix-bits" : number,
        "interface" : number,
    },
    "local" : {
        "port" : string|number,
        "address" : string,
        "prefix-bits" : number,
        "interface" : number,
    }
}
],

"actions-update" : [
{
    "label" : string,
    "flags" : {
        "disabled" : true|false
    }
}
]

"user-values" : {
    "user-null" : null,
    "user-bool" : true,
    "user-bool2" : false,
    "user-uint" : 1,
    "user-sint" : -1,
    "user-float" : 1.0,
    "user-string" : "hello",

    "user-uint2" : { "uint" : 1 },
    "user-sint2" : { "sint", -1 },
    "user-float2" : { "float", 1.0 },
    "user-string2" : { "string", "hello" },
    "user-base64" : { "base64", "aGVsbG8K" }
}
}

*/


struct wolfsentry_json_process_state {
    uint32_t config_version;

    wolfsentry_config_load_flags_t load_flags;

    enum { T_U_C_NONE = 0, T_U_C_TOPCONFIG, T_U_C_EVENTS, T_U_C_DEFAULTPOLICIES, T_U_C_STATIC_ROUTES, T_U_C_ACTIONS, T_U_C_USER_VALUES } table_under_construction;

    enum { O_U_C_NONE = 0, O_U_C_SKIPLEVEL, O_U_C_ROUTE, O_U_C_EVENT, O_U_C_ACTION, O_U_C_USER_VALUE } object_under_construction;

    enum { S_U_C_NONE = 0, S_U_C_EVENTCONFIG, S_U_C_FLAGS, S_U_C_ACTION_LIST, S_U_C_REMOTE_ENDPOINT, S_U_C_LOCAL_ENDPOINT, S_U_C_ROUTE_METADATA, S_U_C_USER_VALUE_JSON } section_under_construction;

    int cur_depth;
    char cur_keyname[WOLFSENTRY_MAX_LABEL_BYTES];
    int cur_keydepth;
    JSON_INPUT_POS key_pos;
#if defined(WOLFSENTRY_HAVE_JSON_DOM) && defined(WOLFSENTRY_JSON_VALUE_MAX_BYTES)
    JSON_INPUT_POS json_value_start_pos;
#endif

    wolfsentry_errcode_t fini_ret;

    const struct wolfsentry_host_platform_interface *hpi;

    struct wolfsentry_eventconfig default_config;
    wolfsentry_action_res_t default_policy;
    struct wolfsentry_event *default_event;

    JSON_PARSER parser;
    struct wolfsentry_context *wolfsentry_actual, *wolfsentry;
#ifdef WOLFSENTRY_HAVE_JSON_DOM
    unsigned int dom_parser_flags;
    JSON_DOM_PARSER dom_parser; /* has a duplicate JSON_PARSER in it that is not used, except for its .wolfsentry_context member. */
#endif
#ifdef WOLFSENTRY_THREADSAFE
    struct wolfsentry_thread_context *thread;
#define JPS_WOLFSENTRY_CONTEXT_ARGS_OUT jps->wolfsentry, jps->thread
#define JPSP_WOLFSENTRY_CONTEXT_ARGS_OUT (*jps)->wolfsentry, (*jps)->thread
#define JPSP_P_WOLFSENTRY_CONTEXT_ARGS_OUT &(*jps)->wolfsentry, (*jps)->thread
#define JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT (*jps)->wolfsentry_actual, (*jps)->thread
#else
#define JPS_WOLFSENTRY_CONTEXT_ARGS_OUT jps->wolfsentry
#define JPSP_WOLFSENTRY_CONTEXT_ARGS_OUT (*jps)->wolfsentry
#define JPSP_P_WOLFSENTRY_CONTEXT_ARGS_OUT &(*jps)->wolfsentry
#define JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT (*jps)->wolfsentry_actual
#endif

    union {
        struct {
            char event_label[WOLFSENTRY_MAX_LABEL_BYTES];
            int event_label_len;
            void *caller_arg; /* xxx */
            WOLFSENTRY_SOCKADDR(WOLFSENTRY_MAX_ADDR_BITS) remote;
            WOLFSENTRY_SOCKADDR(WOLFSENTRY_MAX_ADDR_BITS) local;
            wolfsentry_route_flags_t flags;
        } route;
        struct {
            wolfsentry_priority_t priority;
            char label[WOLFSENTRY_MAX_LABEL_BYTES];
            int label_len;
            struct wolfsentry_eventconfig config;
            int configed;
            int inserted;
        } event;
        struct {
            char label[WOLFSENTRY_MAX_LABEL_BYTES];
            int label_len;
            wolfsentry_action_flags_t flags;
        } action;
        struct {
            char label[WOLFSENTRY_MAX_LABEL_BYTES];
            int label_len;
        } user_value;
    } o_u_c;
};

static wolfsentry_errcode_t reset_o_u_c(struct wolfsentry_json_process_state *jps) {
    switch (jps->object_under_construction) {
    case O_U_C_NONE:
    case O_U_C_SKIPLEVEL:
        WOLFSENTRY_RETURN_OK;
    case O_U_C_ROUTE:
    case O_U_C_ACTION:
    case O_U_C_EVENT:
    case O_U_C_USER_VALUE:
        break;
    default:
        WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);
    }

    memset(&jps->o_u_c, 0, sizeof jps->o_u_c);
    jps->object_under_construction = O_U_C_NONE;
    jps->section_under_construction = S_U_C_NONE;

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_uint64(JSON_TYPE type, const unsigned char *data, size_t data_size, uint64_t *out) {
    char buf[24];
    char *endptr;
    unsigned long long conv;

    /* allow JSON_STRING to accommodate hex and octal values. */
    if ((type != JSON_NUMBER) && (type != JSON_STRING))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (data_size >= sizeof buf)
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    memcpy(buf, data, data_size);
    buf[data_size] = 0;

    errno = 0;
    conv = strtoull(buf, &endptr, 0);
    if (errno != 0)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if ((size_t)(endptr - buf) != data_size)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    *out = (uint64_t)conv;

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_sint64(JSON_TYPE type, const unsigned char *data, size_t data_size, int64_t *out) {
    char buf[24];
    char *endptr;
    long long conv;

    /* allow JSON_STRING to accommodate hex and octal values. */
    if ((type != JSON_NUMBER) && (type != JSON_STRING))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (data_size >= sizeof buf)
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    memcpy(buf, data, data_size);
    buf[data_size] = 0;

    errno = 0;
    conv = strtoll(buf, &endptr, 0);
    if (errno != 0)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if ((size_t)(endptr - buf) != data_size)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    *out = (int64_t)conv;

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_uint32(JSON_TYPE type, const unsigned char *data, size_t data_size, uint32_t *out) {
    char buf[16];
    char *endptr;
    unsigned long conv;

    /* allow JSON_STRING to accommodate hex and octal values. */
    if ((type != JSON_NUMBER) && (type != JSON_STRING))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (data_size >= sizeof buf)
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    memcpy(buf, data, data_size);
    buf[data_size] = 0;

    errno = 0;
    conv = strtoul(buf, &endptr, 0);
    if (errno != 0)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if ((size_t)(endptr - buf) != data_size)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    if (conv > MAX_UINT_OF(*out))
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    *out = (uint32_t)conv;

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_uint16(JSON_TYPE type, const unsigned char *data, size_t data_size, uint16_t *out) {
    char buf[8];
    char *endptr;
    unsigned long conv;

    /* allow JSON_STRING to accommodate hex and octal values. */
    if ((type != JSON_NUMBER) && (type != JSON_STRING))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (data_size >= sizeof buf)
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    memcpy(buf, data, data_size);
    buf[data_size] = 0;

    errno = 0;
    conv = strtoul(buf, &endptr, 0);
    if (errno != 0)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if ((size_t)(endptr - buf) != data_size)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    if (conv > MAX_UINT_OF(*out))
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    *out = (uint16_t)conv;

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_uint8(JSON_TYPE type, const unsigned char *data, size_t data_size, uint8_t *out) {
    char buf[4];
    char *endptr;
    unsigned long conv;

    /* allow JSON_STRING to accommodate hex and octal values. */
    if ((type != JSON_NUMBER) && (type != JSON_STRING))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (data_size >= sizeof buf)
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    memcpy(buf, data, data_size);
    buf[data_size] = 0;

    errno = 0;
    conv = strtoul(buf, &endptr, 0);
    if (errno != 0)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if ((size_t)(endptr - buf) != data_size)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    if (conv > MAX_UINT_OF(*out))
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    *out = (uint8_t)conv;

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_double(JSON_TYPE type, const unsigned char *data, size_t data_size, double *out) {
    char buf[24];
    char *endptr;
    double conv;

    /* allow JSON_STRING to accommodate hex, octal, Inf, NaN, etc. */
    if ((type != JSON_NUMBER) && (type != JSON_STRING))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (data_size >= sizeof buf)
        WOLFSENTRY_ERROR_RETURN(NUMERIC_ARG_TOO_BIG);
    memcpy(buf, data, data_size);
    buf[data_size] = 0;

    errno = 0;
    conv = strtod(buf, &endptr);
    if (errno != 0)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if ((size_t)(endptr - buf) != data_size)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    *out = conv;

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_eventconfig_flag(JSON_TYPE type, wolfsentry_eventconfig_flags_t *flags, wolfsentry_eventconfig_flags_t flag) {
    if (type == JSON_FALSE)
        *flags &= ~flag;
    else if (type == JSON_TRUE)
        *flags |= flag;
    else
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_wolfsentry_duration(struct wolfsentry_context *wolfsentry, JSON_TYPE type, const unsigned char *data, size_t data_size, wolfsentry_time_t *out) {
    wolfsentry_errcode_t ret;
    char *endptr;
    long conv;

    if ((type != JSON_STRING) && (type != JSON_NUMBER))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    errno = 0;
    conv = strtol((const char *)data, &endptr, 0);
    if (errno != 0)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    switch (*endptr) {
    case 'd':
        conv *= 24;
        /* fallthrough */
    case 'h':
        conv *= 60;
        /* fallthrough */
    case 'm':
        conv *= 60;
        /* fallthrough */
    case 's':
        ++endptr;
        break;
    }
    if ((size_t)(endptr - (char *)data) != data_size)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    if ((ret = wolfsentry_interval_from_seconds(wolfsentry, conv, 0 /* howlong_nsecs */, out)) < 0)
        WOLFSENTRY_ERROR_RERETURN(ret);
    else
        WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t convert_default_policy(JSON_TYPE type, const unsigned char *data, size_t data_size, wolfsentry_action_res_t *default_policy) {
    if (type != JSON_STRING)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (streq(data, "accept", data_size))
        *default_policy = WOLFSENTRY_ACTION_RES_ACCEPT|WOLFSENTRY_ACTION_RES_STOP;
    else if (streq(data, "reject", data_size))
        *default_policy = WOLFSENTRY_ACTION_RES_REJECT|WOLFSENTRY_ACTION_RES_STOP;
    else if (streq(data, "reset", data_size))
        *default_policy = WOLFSENTRY_ACTION_RES_REJECT|WOLFSENTRY_ACTION_RES_PORT_RESET|WOLFSENTRY_ACTION_RES_STOP;
    else
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t handle_eventconfig_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size, struct wolfsentry_eventconfig *eventconfig) {
    if (type == JSON_OBJECT_END) {
        wolfsentry_errcode_t ret = wolfsentry_defaultconfig_update(jps->wolfsentry, &jps->default_config);
        jps->table_under_construction = T_U_C_NONE;
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        if (jps->default_policy) {
            struct wolfsentry_route_table *routes;
            ret = wolfsentry_route_get_main_table(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, &routes);
            WOLFSENTRY_RERETURN_IF_ERROR(ret);
            ret = wolfsentry_route_table_default_policy_set(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, routes, jps->default_policy);
            WOLFSENTRY_RERETURN_IF_ERROR(ret);
        }
        WOLFSENTRY_RETURN_OK;
    }
    if (! strcmp(jps->cur_keyname, "max-connection-count"))
        WOLFSENTRY_ERROR_RERETURN(convert_uint32(type, data, data_size, &eventconfig->max_connection_count));
    if (! strcmp(jps->cur_keyname, "penalty-box-duration"))
        WOLFSENTRY_ERROR_RERETURN(convert_wolfsentry_duration(jps->wolfsentry, type, data, data_size, &eventconfig->penaltybox_duration));
    if (! strcmp(jps->cur_keyname, "route-idle-time-for-purge"))
        WOLFSENTRY_ERROR_RERETURN(convert_wolfsentry_duration(jps->wolfsentry, type, data, data_size, &eventconfig->route_idle_time_for_purge));
    if (! strcmp(jps->cur_keyname, "derog-thresh-for-penalty-boxing"))
        WOLFSENTRY_ERROR_RERETURN(convert_uint32(type, data, data_size, &eventconfig->derogatory_threshold_for_penaltybox));
    if (! strcmp(jps->cur_keyname, "derog-thresh-ignore-commendable"))
        WOLFSENTRY_ERROR_RERETURN(convert_eventconfig_flag(type, &eventconfig->flags, WOLFSENTRY_EVENTCONFIG_FLAG_DEROGATORY_THRESHOLD_IGNORE_COMMENDABLE));
    if (! strcmp(jps->cur_keyname, "commendable-clears-derogatory"))
        WOLFSENTRY_ERROR_RERETURN(convert_eventconfig_flag(type, &eventconfig->flags, WOLFSENTRY_EVENTCONFIG_FLAG_COMMENDABLE_CLEARS_DEROGATORY));

    if (! strcmp(jps->cur_keyname, "max-purgeable-routes")) {
        struct wolfsentry_route_table *route_table;
        wolfsentry_hitcount_t max_purgeable_routes;
        wolfsentry_errcode_t ret;
        if ((ret = convert_uint32(type, data, data_size, &max_purgeable_routes)) < 0)
            WOLFSENTRY_ERROR_RERETURN(ret);
        if (jps->table_under_construction != T_U_C_TOPCONFIG)
            WOLFSENTRY_ERROR_RETURN(CONFIG_MISPLACED_KEY);
        ret = wolfsentry_route_get_main_table(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, &route_table);
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        if ((ret = wolfsentry_route_table_max_purgeable_routes_set(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, route_table, max_purgeable_routes)) < 0)
            WOLFSENTRY_ERROR_RERETURN(ret);
        WOLFSENTRY_RETURN_OK;
    }

    WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);
}

static wolfsentry_errcode_t handle_defaultpolicy_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size) {
    if (jps->table_under_construction != T_U_C_DEFAULTPOLICIES)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);
    if (type == JSON_OBJECT_END) {
        wolfsentry_errcode_t ret = wolfsentry_defaultconfig_update(jps->wolfsentry, &jps->default_config);
        jps->table_under_construction = T_U_C_NONE;
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        if (jps->default_policy) {
            struct wolfsentry_route_table *routes;
            ret = wolfsentry_route_get_main_table(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, &routes);
            WOLFSENTRY_RERETURN_IF_ERROR(ret);
            ret = wolfsentry_route_table_default_policy_set(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, routes, jps->default_policy);
            WOLFSENTRY_RERETURN_IF_ERROR(ret);
        }
        WOLFSENTRY_RETURN_OK;
    }
    if (! strcmp(jps->cur_keyname, "default-policy"))
        WOLFSENTRY_ERROR_RERETURN(convert_default_policy(type, data, data_size, &jps->default_policy));
    if (! strcmp(jps->cur_keyname, "default-event")) {
        struct wolfsentry_route_table *routes;
        if (WOLFSENTRY_CHECK_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_ROUTES_OR_EVENTS))
            WOLFSENTRY_RETURN_OK;
        wolfsentry_errcode_t ret = wolfsentry_route_get_main_table(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, &routes);
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        WOLFSENTRY_ERROR_RERETURN(wolfsentry_route_table_set_default_event(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, routes, (const char *)data, (int)data_size));
    }
    WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);
}

static inline int convert_hex_digit(unsigned char digit) {
    if ((digit >= '0') && (digit <= '9'))
        return digit - '0';
    else if ((digit >= 'A') && (digit <= 'F'))
        return digit - 'A' + 10;
    else if ((digit >= 'a') && (digit <= 'f'))
        return digit - 'a' + 10;
    else
        return -1;
}

static inline int convert_hex_byte(const unsigned char **in, size_t *in_len, byte *out) {
    int d1, d2;
    if (*in_len < 2)
        return -1;
    d1 = convert_hex_digit(*(*in)++);
    if (d1 < 0)
        return d1;
    d2 = convert_hex_digit(*(*in)++);
    if (d2 < 0)
        return d2;
    *out = (byte)((d1 << 4) | d2);
    *in_len -= 2;
    return 0;
}

static wolfsentry_errcode_t convert_sockaddr_address(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size, struct wolfsentry_sockaddr *sa) {
    if (type != JSON_STRING)
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (sa->sa_family == WOLFSENTRY_AF_LINK) {
        int n;
        for (n=0; ;) {
            if (n == 8)
                WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
            if (convert_hex_byte(&data, &data_size, sa->addr + n) < 0)
                WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
            ++n;
            if (data_size == 0) {
                if ((n != 6) && (n != 8))
                    WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
                if (sa->addr_len == 0)
                    sa->addr_len = (wolfsentry_addr_bits_t)(n * 8);
                WOLFSENTRY_RETURN_OK;
            }
            if (*data++ != ':')
                WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
            --data_size;
        }

        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    }
    else if (sa->sa_family == WOLFSENTRY_AF_INET) {
        char d_buf[16];
        if (data_size >= sizeof d_buf)
            WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
        memcpy(d_buf, data, data_size);
        d_buf[data_size] = 0;
        switch (inet_pton(AF_INET, d_buf, sa->addr)) {
        case 1:
            if (sa->addr_len == 0)
                sa->addr_len = 32;
            WOLFSENTRY_RETURN_OK;
        case 0:
            WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
        case -1:
        default:
            WOLFSENTRY_ERROR_RETURN(SYS_OP_FATAL);
        }
    }
    else if (sa->sa_family == WOLFSENTRY_AF_INET6) {
        char d_buf[64];
        if (data_size >= sizeof d_buf)
            WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
        memcpy(d_buf, data, data_size);
        d_buf[data_size] = 0;
        switch (inet_pton(AF_INET6, d_buf, sa->addr)) {
        case 1:
            if (sa->addr_len == 0)
                sa->addr_len = 128;
            WOLFSENTRY_RETURN_OK;
        case 0:
            WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
        case -1:
        default:
            WOLFSENTRY_ERROR_RETURN(SYS_OP_FATAL);
        }
    }
    else {
        wolfsentry_errcode_t ret;
        wolfsentry_addr_family_parser_t parser;
        if ((ret = wolfsentry_addr_family_get_parser(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, sa->sa_family, &parser)) < 0)
            WOLFSENTRY_ERROR_RETURN(CONFIG_MISSING_HANDLER);
        sa->addr_len = WOLFSENTRY_MAX_ADDR_BITS;
        WOLFSENTRY_ERROR_RERETURN(parser(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, (const char *)data, (int)data_size, sa->addr, &sa->addr_len));
    }
}

#ifndef WOLFSENTRY_NO_GETPROTOBY

static wolfsentry_errcode_t convert_sockaddr_port_name(struct wolfsentry_json_process_state *jps, const unsigned char *data, size_t data_size, struct wolfsentry_sockaddr *sa) {
    char d_buf[64];
    struct servent *s;
    struct protoent *p;

    if (! WOLFSENTRY_CHECK_BITS(jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_TCPLIKE_PORT_NUMBERS))
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (data_size >= sizeof d_buf)
        WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);

    memcpy(d_buf, data, data_size);
    d_buf[data_size] = 0;

    if (sa->sa_proto != 0)
        p = getprotobynumber(sa->sa_proto);
    else
        p = NULL;

    s = getservbyname(d_buf, p ? p->p_name : NULL);
    if (s == NULL)
        WOLFSENTRY_ERROR_RETURN(ITEM_NOT_FOUND);
    else {
        sa->sa_port = (wolfsentry_port_t)ntohs((uint16_t)s->s_port);
        WOLFSENTRY_RETURN_OK;
    }
}

#endif

static wolfsentry_errcode_t handle_route_endpoint_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size, struct wolfsentry_sockaddr *sa) {
    if (! strcmp(jps->cur_keyname, "port")) {
        WOLFSENTRY_CLEAR_BITS(jps->o_u_c.route.flags,
                              sa == (struct wolfsentry_sockaddr *)&jps->o_u_c.route.remote ?
                              WOLFSENTRY_ROUTE_FLAG_SA_REMOTE_PORT_WILDCARD :
                              WOLFSENTRY_ROUTE_FLAG_SA_LOCAL_PORT_WILDCARD);
        if (type == JSON_NUMBER)
            WOLFSENTRY_ERROR_RERETURN(convert_uint16(type, data, data_size, &sa->sa_port));
#ifndef WOLFSENTRY_NO_GETPROTOBY
        else if (type == JSON_STRING)
            WOLFSENTRY_ERROR_RERETURN(convert_sockaddr_port_name(jps, data, data_size, sa));
#endif
        else
            WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
    } else if (! strcmp(jps->cur_keyname, "address")) {
        WOLFSENTRY_CLEAR_BITS(jps->o_u_c.route.flags,
                              sa == (struct wolfsentry_sockaddr *)&jps->o_u_c.route.remote ?
                              WOLFSENTRY_ROUTE_FLAG_SA_REMOTE_ADDR_WILDCARD :
                              WOLFSENTRY_ROUTE_FLAG_SA_LOCAL_ADDR_WILDCARD);
        WOLFSENTRY_ERROR_RERETURN(convert_sockaddr_address(jps, type, data, data_size, sa));
    } else if (! strcmp(jps->cur_keyname, "prefix-bits"))
        WOLFSENTRY_ERROR_RERETURN(convert_uint16(type, data, data_size, &sa->addr_len));
    else if (! strcmp(jps->cur_keyname, "interface")) {
        WOLFSENTRY_CLEAR_BITS(jps->o_u_c.route.flags,
                              sa == (struct wolfsentry_sockaddr *)&jps->o_u_c.route.remote ?
                              WOLFSENTRY_ROUTE_FLAG_REMOTE_INTERFACE_WILDCARD :
                              WOLFSENTRY_ROUTE_FLAG_LOCAL_INTERFACE_WILDCARD);
        WOLFSENTRY_ERROR_RERETURN(convert_uint8(type, data, data_size, &sa->interface));
    } else
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);
}

static wolfsentry_errcode_t handle_route_boolean_clause(JSON_TYPE type, wolfsentry_route_flags_t *flags, wolfsentry_route_flags_t bit) {
    if (type == JSON_TRUE) {
        WOLFSENTRY_SET_BITS(*flags, bit);
        WOLFSENTRY_RETURN_OK;
    } else if (type == JSON_FALSE) {
        WOLFSENTRY_CLEAR_BITS(*flags, bit);
        WOLFSENTRY_RETURN_OK;
    } else
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
}

static wolfsentry_errcode_t handle_route_family_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size) {
    WOLFSENTRY_CLEAR_BITS(jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_SA_FAMILY_WILDCARD);

    if (type == JSON_NUMBER) {
        wolfsentry_errcode_t ret = convert_uint16(type, data, data_size, &jps->o_u_c.route.remote.sa_family);
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        jps->o_u_c.route.local.sa_family = jps->o_u_c.route.remote.sa_family;
        WOLFSENTRY_RETURN_OK;
    }
#ifdef WOLFSENTRY_PROTOCOL_NAMES
    else if (type == JSON_STRING) {
        wolfsentry_errcode_t ret;
        ret = wolfsentry_addr_family_pton(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, (const char *)data, (int)data_size, &jps->o_u_c.route.local.sa_family);
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        jps->o_u_c.route.remote.sa_family = jps->o_u_c.route.local.sa_family;
        WOLFSENTRY_ERROR_RERETURN(ret);
    }
#endif
    else
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
}

static wolfsentry_errcode_t handle_route_protocol_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size) {
    WOLFSENTRY_CLEAR_BITS(jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_SA_PROTO_WILDCARD);

    if (type == JSON_NUMBER) {
        wolfsentry_errcode_t ret = convert_uint16(type, data, data_size, &jps->o_u_c.route.remote.sa_proto);
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        jps->o_u_c.route.local.sa_proto = jps->o_u_c.route.remote.sa_proto;
    }
#ifndef WOLFSENTRY_NO_GETPROTOBY
    else if (type == JSON_STRING) {
        char d_buf[64];
        struct protoent *p;

        if ((jps->o_u_c.route.remote.sa_family != WOLFSENTRY_AF_INET) &&
            (jps->o_u_c.route.remote.sa_family != WOLFSENTRY_AF_INET6))
            WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

        if (data_size >= sizeof d_buf)
            WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);

        memcpy(d_buf, data, data_size);
        d_buf[data_size] = 0;

        p = getprotobyname(d_buf);
        if (p == NULL)
            WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
        else {
            jps->o_u_c.route.remote.sa_proto = (wolfsentry_proto_t)p->p_proto;
            jps->o_u_c.route.local.sa_proto = (wolfsentry_proto_t)p->p_proto;
        }
    }
#endif
    else
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

    if (((jps->o_u_c.route.remote.sa_family == WOLFSENTRY_AF_INET) || (jps->o_u_c.route.remote.sa_family == WOLFSENTRY_AF_INET6)) &&
        ((jps->o_u_c.route.remote.sa_proto == IPPROTO_TCP) || (jps->o_u_c.route.remote.sa_proto == IPPROTO_UDP)))
        WOLFSENTRY_SET_BITS(jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_TCPLIKE_PORT_NUMBERS);

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t handle_route_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size) {
    wolfsentry_errcode_t ret;
    if ((jps->cur_depth == 2) && (type == JSON_OBJECT_END)) {
        wolfsentry_ent_id_t id;
        wolfsentry_action_res_t action_results;
        if (WOLFSENTRY_CHECK_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_ROUTES_OR_EVENTS))
            ret = WOLFSENTRY_ERROR_ENCODE(OK);
        else
            ret = wolfsentry_route_insert(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT,
                jps->o_u_c.route.caller_arg,
                (const struct wolfsentry_sockaddr *)&jps->o_u_c.route.remote,
                (const struct wolfsentry_sockaddr *)&jps->o_u_c.route.local,
                jps->o_u_c.route.flags,
                (jps->o_u_c.route.event_label_len > 0) ? jps->o_u_c.route.event_label : NULL,
                jps->o_u_c.route.event_label_len,
                &id,
                &action_results);
        reset_o_u_c(jps);
        WOLFSENTRY_ERROR_RERETURN(ret);
    }
    if ((jps->cur_depth == 3) && (type == JSON_OBJECT_BEG)) {
        reset_o_u_c(jps);
        jps->object_under_construction = O_U_C_ROUTE;
        /* speculatively set all the wildcard fields, then clear them piecemeal as directives provide. */
        WOLFSENTRY_SET_BITS(jps->o_u_c.route.flags,
                            WOLFSENTRY_ROUTE_FLAG_REMOTE_INTERFACE_WILDCARD|
                            WOLFSENTRY_ROUTE_FLAG_LOCAL_INTERFACE_WILDCARD|
                            WOLFSENTRY_ROUTE_FLAG_SA_FAMILY_WILDCARD|
                            WOLFSENTRY_ROUTE_FLAG_SA_REMOTE_ADDR_WILDCARD|
                            WOLFSENTRY_ROUTE_FLAG_SA_LOCAL_ADDR_WILDCARD|
                            WOLFSENTRY_ROUTE_FLAG_SA_PROTO_WILDCARD|
                            WOLFSENTRY_ROUTE_FLAG_SA_REMOTE_PORT_WILDCARD|
                            WOLFSENTRY_ROUTE_FLAG_SA_LOCAL_PORT_WILDCARD);
        WOLFSENTRY_RETURN_OK;
    }
    if (jps->cur_depth == 4) {
        if (type == JSON_OBJECT_BEG) {
            if (! strcmp(jps->cur_keyname, "remote")) {
                jps->section_under_construction = S_U_C_REMOTE_ENDPOINT;
                WOLFSENTRY_RETURN_OK;
            } else if (! strcmp(jps->cur_keyname, "local")) {
                jps->section_under_construction = S_U_C_LOCAL_ENDPOINT;
                WOLFSENTRY_RETURN_OK;
            } else
                WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);
        } else if (jps->section_under_construction == S_U_C_REMOTE_ENDPOINT)
            WOLFSENTRY_ERROR_RERETURN(handle_route_endpoint_clause(jps, type, data, data_size, (struct wolfsentry_sockaddr *)&jps->o_u_c.route.remote));
        else if (jps->section_under_construction == S_U_C_LOCAL_ENDPOINT)
            WOLFSENTRY_ERROR_RERETURN(handle_route_endpoint_clause(jps, type, data, data_size, (struct wolfsentry_sockaddr *)&jps->o_u_c.route.local));
        else
            WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED); /* can't happen. */
    }
    if ((jps->cur_depth == 3) && (type == JSON_OBJECT_END)) {
        jps->section_under_construction = S_U_C_NONE;
        WOLFSENTRY_RETURN_OK;
    }

    if (jps->cur_depth != 3)
        WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);

    if (! strcmp(jps->cur_keyname, "parent-event")) {
        if (data_size > sizeof jps->o_u_c.route.event_label)
            WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);
        jps->o_u_c.route.event_label_len = (int)data_size;
        memcpy(jps->o_u_c.route.event_label, data, data_size);
        WOLFSENTRY_CLEAR_BITS(jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_PARENT_EVENT_WILDCARD);
    }
    else if (! strcmp(jps->cur_keyname, "family"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_family_clause(jps, type, data, data_size));
    else if (! strcmp(jps->cur_keyname, "protocol"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_protocol_clause(jps, type, data, data_size));
    else if (! strcmp(jps->cur_keyname, "tcplike-port-numbers"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_TCPLIKE_PORT_NUMBERS));
    else if (! strcmp(jps->cur_keyname, "direction-in"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_DIRECTION_IN));
    else if (! strcmp(jps->cur_keyname, "direction-out"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_DIRECTION_OUT));
    else if (! strcmp(jps->cur_keyname, "penalty-boxed"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_PENALTYBOXED));
    else if (! strcmp(jps->cur_keyname, "green-listed"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_GREENLISTED));
    else if (! strcmp(jps->cur_keyname, "dont-count-hits"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_DONT_COUNT_HITS));
    else if (! strcmp(jps->cur_keyname, "dont-count-current-connections"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_DONT_COUNT_CURRENT_CONNECTIONS));
    else if (! strcmp(jps->cur_keyname, "port-reset"))
        WOLFSENTRY_ERROR_RERETURN(handle_route_boolean_clause(type, &jps->o_u_c.route.flags, WOLFSENTRY_ROUTE_FLAG_PORT_RESET));
    else
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);

    WOLFSENTRY_RETURN_OK;
}

static wolfsentry_errcode_t handle_event_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE type, const unsigned char *data, size_t data_size) {
    if (jps->table_under_construction != T_U_C_EVENTS)
        WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);

    if ((jps->cur_depth == 1) && (type == JSON_ARRAY_END)) {
        jps->table_under_construction = T_U_C_NONE;
        WOLFSENTRY_RETURN_OK;
    }

    if (jps->object_under_construction != O_U_C_EVENT) {
        if ((jps->cur_depth == 3) && (type == JSON_OBJECT_BEG) && (jps->object_under_construction == O_U_C_NONE)) {
            jps->object_under_construction = O_U_C_EVENT;
            WOLFSENTRY_RETURN_OK;
        } else
            WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);
    }

    if ((jps->cur_depth == 3) && (type == JSON_STRING) && (! strcmp(jps->cur_keyname, "label"))) {
        if (data_size >= sizeof jps->o_u_c.event.label)
            WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
        memcpy(jps->o_u_c.event.label, data, data_size);
        jps->o_u_c.event.label_len = (int)data_size;
        WOLFSENTRY_RETURN_OK;
    }

    if ((jps->cur_depth == 3) && (type == JSON_NUMBER)) {
        if (! strcmp(jps->cur_keyname, "priority")) {
            if (jps->o_u_c.event.inserted)
                WOLFSENTRY_ERROR_RETURN(CONFIG_OUT_OF_SEQUENCE);
            WOLFSENTRY_ERROR_RERETURN(convert_uint16(type, data, data_size, &jps->o_u_c.event.priority));
        }
        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);
    }

    if ((jps->cur_depth == 4) && (type == JSON_OBJECT_BEG) && (! strcmp(jps->cur_keyname, "config")) && (jps->section_under_construction == S_U_C_NONE)) {
        if (jps->o_u_c.event.inserted)
            WOLFSENTRY_ERROR_RETURN(CONFIG_OUT_OF_SEQUENCE);
        jps->section_under_construction = S_U_C_EVENTCONFIG;
        jps->o_u_c.event.config = jps->default_config; /* initialize with global defaults, particularly to pick up route_private_data* fields */
        jps->o_u_c.event.configed = 1;
        WOLFSENTRY_RETURN_OK;
    }

    if ((jps->cur_depth == 4) && (jps->section_under_construction == S_U_C_EVENTCONFIG))
        WOLFSENTRY_ERROR_RERETURN(handle_eventconfig_clause(jps, type, data, data_size, &jps->o_u_c.event.config));

    if ((jps->cur_depth == 3) && (type == JSON_OBJECT_END) && (jps->section_under_construction == S_U_C_EVENTCONFIG)) {
        jps->section_under_construction = S_U_C_NONE;
        if (WOLFSENTRY_CHECK_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_ROUTES_OR_EVENTS))
            WOLFSENTRY_RETURN_OK;
        else if (jps->o_u_c.event.inserted)
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_event_update_config(JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, jps->o_u_c.event.label, jps->o_u_c.event.label_len, &jps->o_u_c.event.config));
        else
            WOLFSENTRY_RETURN_OK;
    }

    if (! jps->o_u_c.event.inserted) {
        wolfsentry_ent_id_t id;
        wolfsentry_errcode_t ret;
        if (WOLFSENTRY_CHECK_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_ROUTES_OR_EVENTS))
            ret = WOLFSENTRY_ERROR_ENCODE(OK);
        else
            ret = wolfsentry_event_insert(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT,
                jps->o_u_c.event.label,
                jps->o_u_c.event.label_len,
                jps->o_u_c.event.priority,
                jps->o_u_c.event.configed ? &jps->o_u_c.event.config : NULL,
                WOLFSENTRY_EVENT_FLAG_NONE,
                &id);
        WOLFSENTRY_RERETURN_IF_ERROR(ret);
        jps->o_u_c.event.inserted = 1;
    }

    if ((jps->cur_depth == 2) && (type == JSON_OBJECT_END))
        WOLFSENTRY_ERROR_RERETURN(reset_o_u_c(jps));

    if ((jps->cur_depth == 3) && (type == JSON_STRING)) {
        wolfsentry_action_type_t subevent_type = WOLFSENTRY_ACTION_TYPE_NONE;

        if (! strcmp(jps->cur_keyname, "insert-event"))
            subevent_type = WOLFSENTRY_ACTION_TYPE_INSERT;
        else if (! strcmp(jps->cur_keyname, "match-event"))
            subevent_type = WOLFSENTRY_ACTION_TYPE_MATCH;
        else if (! strcmp(jps->cur_keyname, "update-event"))
            subevent_type = WOLFSENTRY_ACTION_TYPE_UPDATE;
        else if (! strcmp(jps->cur_keyname, "delete-event"))
            subevent_type = WOLFSENTRY_ACTION_TYPE_DELETE;
        else if (! strcmp(jps->cur_keyname, "decision-event"))
            subevent_type = WOLFSENTRY_ACTION_TYPE_DECISION;

        if (subevent_type != WOLFSENTRY_ACTION_TYPE_NONE) {
            if (data_size > WOLFSENTRY_MAX_LABEL_BYTES)
                WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
            if (WOLFSENTRY_CHECK_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_ROUTES_OR_EVENTS))
                WOLFSENTRY_RETURN_OK;
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_event_set_subevent(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT,
                jps->o_u_c.event.label,
                jps->o_u_c.event.label_len,
                subevent_type,
                (const char *)data,
                (int)data_size));
        }

        WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_KEY);
    }

    if ((jps->cur_depth == 4) && (type == JSON_ARRAY_BEG) && (jps->section_under_construction == S_U_C_NONE) && (! strcmp(jps->cur_keyname, "actions"))) {
        jps->section_under_construction = S_U_C_ACTION_LIST;
        WOLFSENTRY_RETURN_OK;
    }

    if ((jps->cur_depth == 4) && (jps->section_under_construction == S_U_C_ACTION_LIST) && (type == JSON_STRING)) {
        if (data_size > WOLFSENTRY_MAX_LABEL_BYTES)
            WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
        if (WOLFSENTRY_CHECK_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_ROUTES_OR_EVENTS))
            WOLFSENTRY_RETURN_OK;
        WOLFSENTRY_ERROR_RERETURN(wolfsentry_event_action_append(
                    JPS_WOLFSENTRY_CONTEXT_ARGS_OUT,
                    jps->o_u_c.event.label,
                    jps->o_u_c.event.label_len,
                    (const char *)data,
                    (int)data_size));
    }

    if ((jps->cur_depth == 3) && (type == JSON_ARRAY_END) && (jps->section_under_construction == S_U_C_ACTION_LIST)) {
        jps->section_under_construction = S_U_C_NONE;
        WOLFSENTRY_RETURN_OK;
    }

    WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);
}

static wolfsentry_errcode_t handle_user_value_clause(struct wolfsentry_json_process_state *jps, JSON_TYPE json_type, const unsigned char *data, size_t data_size) {
    wolfsentry_errcode_t ret;

    if (jps->table_under_construction != T_U_C_USER_VALUES)
        WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);

    if ((jps->cur_depth == 1) && (json_type == JSON_OBJECT_END)) {
        jps->table_under_construction = T_U_C_NONE;
        WOLFSENTRY_RETURN_OK;
    }

    if ((jps->cur_depth == 2) && (jps->object_under_construction == O_U_C_USER_VALUE) && (json_type == JSON_OBJECT_END))
        WOLFSENTRY_ERROR_RERETURN(reset_o_u_c(jps));

    if ((jps->cur_depth == 3) && (jps->cur_keydepth == 2) && (jps->object_under_construction == O_U_C_NONE)) {
        jps->o_u_c.user_value.label_len = (int)strlen(jps->cur_keyname);
        memcpy(jps->o_u_c.user_value.label, jps->cur_keyname, (size_t)jps->o_u_c.user_value.label_len);
        jps->object_under_construction = O_U_C_USER_VALUE;
        WOLFSENTRY_RETURN_OK;
    }

    if ((jps->cur_depth == 2) && (jps->cur_keydepth == 2) && (jps->object_under_construction == O_U_C_NONE)) {
        switch (json_type) {
        case JSON_NULL:
            ret = wolfsentry_user_value_store_null(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT,
                jps->cur_keyname,
                WOLFSENTRY_LENGTH_NULL_TERMINATED,
                0);
            jps->object_under_construction = O_U_C_NONE;
            WOLFSENTRY_ERROR_RERETURN(ret);
        case JSON_FALSE:
            ret = wolfsentry_user_value_store_bool(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                jps->cur_keyname,
                WOLFSENTRY_LENGTH_NULL_TERMINATED,
                WOLFSENTRY_KV_FALSE,
                0);
            jps->object_under_construction = O_U_C_NONE;
            WOLFSENTRY_ERROR_RERETURN(ret);
        case JSON_TRUE:
            ret = wolfsentry_user_value_store_bool(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                jps->cur_keyname,
                WOLFSENTRY_LENGTH_NULL_TERMINATED,
                WOLFSENTRY_KV_TRUE,
                0);
            jps->object_under_construction = O_U_C_NONE;
            WOLFSENTRY_ERROR_RERETURN(ret);
        case JSON_NUMBER:
            do {
                int64_t i;
                if ((ret = convert_sint64(json_type, data, data_size, &i)) < 0)
                    break;
                ret = wolfsentry_user_value_store_sint(
                    JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                    jps->cur_keyname,
                    WOLFSENTRY_LENGTH_NULL_TERMINATED,
                    i,
                    0);
            } while(0);
            if (! WOLFSENTRY_ERROR_CODE_IS(ret, OK)) {
                double d;
                if ((ret = convert_double(json_type, data, data_size, &d)) < 0)
                    WOLFSENTRY_ERROR_RERETURN(ret);
                ret = wolfsentry_user_value_store_double(
                    JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                    jps->cur_keyname,
                    WOLFSENTRY_LENGTH_NULL_TERMINATED,
                    d,
                    0);
            }
            jps->object_under_construction = O_U_C_NONE;
            WOLFSENTRY_ERROR_RERETURN(ret);
        case JSON_STRING:
            if (data_size >= WOLFSENTRY_KV_MAX_VALUE_BYTES)
                WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
            ret = wolfsentry_user_value_store_string(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                jps->cur_keyname,
                WOLFSENTRY_LENGTH_NULL_TERMINATED,
                (const char *)data,
                (int)data_size,
                0);
            jps->object_under_construction = O_U_C_NONE;
            WOLFSENTRY_ERROR_RERETURN(ret);
        case JSON_OBJECT_BEG:
            jps->o_u_c.user_value.label_len = (int)strlen(jps->cur_keyname);
            memcpy(jps->o_u_c.user_value.label, jps->cur_keyname, (size_t)jps->o_u_c.user_value.label_len);
            WOLFSENTRY_RETURN_OK;
        case JSON_OBJECT_END:
            jps->object_under_construction = O_U_C_NONE;
            WOLFSENTRY_RETURN_OK;
        default:
            WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);
        }
    }

#ifdef WOLFSENTRY_HAVE_JSON_DOM
    if ((jps->object_under_construction == O_U_C_USER_VALUE) && (jps->section_under_construction != S_U_C_USER_VALUE_JSON) && (! strcmp(jps->cur_keyname, "json"))) {
#ifdef WOLFSENTRY_JSON_VALUE_MAX_BYTES
        jps->json_value_start_pos = jps->parser.pos;
#endif
        jps->section_under_construction = S_U_C_USER_VALUE_JSON;
        ret = json_dom_init_1(WOLFSENTRY_CONTEXT_ARGS_OUT_EX4(wolfsentry_get_allocator(jps->wolfsentry), jps->thread), &jps->dom_parser, jps->dom_parser_flags);
        if (ret < 0)
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_centijson_errcode_translate(ret));
    }

    if ((jps->cur_depth >= 3) && (jps->object_under_construction == O_U_C_USER_VALUE) && (jps->section_under_construction == S_U_C_USER_VALUE_JSON)) {

#ifdef WOLFSENTRY_JSON_VALUE_MAX_BYTES
        if (jps->parser.pos.offset - jps->json_value_start_pos.offset > WOLFSENTRY_JSON_VALUE_MAX_BYTES) {
            memcpy(&jps->parser.err_pos, &jps->parser.pos, sizeof jps->parser.err_pos);
            WOLFSENTRY_ERROR_RETURN(CONFIG_JSON_VALUE_SIZE);
        }
#endif

        ret = json_dom_process(json_type, data, data_size, (void *)&jps->dom_parser);

        if (ret < 0) {
            memcpy(&jps->parser.err_pos, &jps->parser.value_pos, sizeof jps->parser.err_pos);
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_centijson_errcode_translate(ret));
        }

        if (jps->cur_depth == 3) {
            JSON_VALUE jv;

            ret = json_dom_fini_aux(&jps->dom_parser, &jv);
            if (ret != 0)
                WOLFSENTRY_ERROR_RERETURN(wolfsentry_centijson_errcode_translate(ret));

            ret = wolfsentry_user_value_store_json(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT,
                jps->o_u_c.user_value.label,
                jps->o_u_c.user_value.label_len,
                &jv,
                0 /* overwrite_p */);

            if (ret < 0) {
                wolfsentry_errcode_t ret2 = json_value_fini(WOLFSENTRY_CONTEXT_ARGS_OUT_EX4(wolfsentry_get_allocator(jps->wolfsentry), jps->thread), &jv);
                if (ret2 < 0)
                    WOLFSENTRY_ERROR_RERETURN(wolfsentry_centijson_errcode_translate(ret2));
                else
                    WOLFSENTRY_ERROR_RERETURN(ret);
            }

            jps->section_under_construction = S_U_C_NONE;
        }
        WOLFSENTRY_RETURN_OK;
    }
#endif /* WOLFSENTRY_HAVE_JSON_DOM */

    if ((jps->cur_depth == 3) && (jps->object_under_construction == O_U_C_USER_VALUE)) {
        wolfsentry_kv_type_t ws_type;

        if (jps->cur_keydepth != 3)
            WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);

        if (! ((json_type == JSON_NUMBER) ||
               (json_type == JSON_STRING)))
            WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);

        if (! strcmp(jps->cur_keyname, "uint"))
            ws_type = WOLFSENTRY_KV_UINT;
        else if (! strcmp(jps->cur_keyname, "sint"))
            ws_type = WOLFSENTRY_KV_SINT;
        else if (! strcmp(jps->cur_keyname, "float"))
            ws_type = WOLFSENTRY_KV_FLOAT;
        else if (! strcmp(jps->cur_keyname, "string"))
            ws_type = WOLFSENTRY_KV_STRING;
        else if (! strcmp(jps->cur_keyname, "base64"))
            ws_type = WOLFSENTRY_KV_BYTES;
        else
            WOLFSENTRY_ERROR_RETURN(CONFIG_INVALID_VALUE);

        switch (ws_type) {
        case WOLFSENTRY_KV_UINT: {
            uint64_t i;
            if ((ret = convert_uint64(json_type, data, data_size, &i)) < 0)
                WOLFSENTRY_ERROR_RERETURN(ret);
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_user_value_store_uint(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT,
                jps->o_u_c.user_value.label,
                jps->o_u_c.user_value.label_len,
                i,
                0));
        }
        case WOLFSENTRY_KV_SINT: {
            int64_t i;
            if ((ret = convert_sint64(json_type, data, data_size, &i)) < 0)
                WOLFSENTRY_ERROR_RERETURN(ret);
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_user_value_store_sint(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                jps->o_u_c.user_value.label,
                jps->o_u_c.user_value.label_len,
                i,
                0));
        }
        case WOLFSENTRY_KV_FLOAT: {
            double d;
            if ((ret = convert_double(json_type, data, data_size, &d)) < 0)
                WOLFSENTRY_ERROR_RERETURN(ret);
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_user_value_store_double(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                jps->o_u_c.user_value.label,
                jps->o_u_c.user_value.label_len,
                d,
                0));
        }
        case WOLFSENTRY_KV_STRING:
            if (data_size >= WOLFSENTRY_KV_MAX_VALUE_BYTES)
                WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_user_value_store_string(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                jps->o_u_c.user_value.label,
                jps->o_u_c.user_value.label_len,
                (const char *)data,
                (int)data_size,
                0));

        case WOLFSENTRY_KV_BYTES:
            if (data_size >= WOLFSENTRY_KV_MAX_VALUE_BYTES)
                WOLFSENTRY_ERROR_RETURN(STRING_ARG_TOO_LONG);
            WOLFSENTRY_ERROR_RERETURN(wolfsentry_user_value_store_bytes_base64(
                JPS_WOLFSENTRY_CONTEXT_ARGS_OUT, 
                jps->o_u_c.user_value.label,
                jps->o_u_c.user_value.label_len,
                (const char *)data,
                (int)data_size,
                0));

        case WOLFSENTRY_KV_JSON:
        case WOLFSENTRY_KV_NONE:
        case WOLFSENTRY_KV_NULL:
        case WOLFSENTRY_KV_TRUE:
        case WOLFSENTRY_KV_FALSE:
        case WOLFSENTRY_KV_FLAG_READONLY:
            WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);
        }

        WOLFSENTRY_RETURN_OK;

    }

    WOLFSENTRY_ERROR_RETURN(CONFIG_UNEXPECTED);
}

#define WOLFSENTRY_ERROR_OUT(x) { ret = WOLFSENTRY_ERROR_ENCODE(x); goto out; }

static wolfsentry_errcode_t json_process(
    JSON_TYPE type,
    const unsigned char *data,
    size_t data_size,
    struct wolfsentry_json_process_state *jps)
{
    wolfsentry_errcode_t ret;

#ifdef DEBUG_JSON
    if (data)
        printf("depth=%d t=%d d=\"%.*s\"\n", jps->cur_depth, type, (int)data_size, data);
    else
        printf("depth=%d t=%d\n", jps->cur_depth, type);
#endif

    if (type == JSON_KEY) {

#ifdef WOLFSENTRY_HAVE_JSON_DOM
        if (jps->section_under_construction == S_U_C_USER_VALUE_JSON) {
            ret = handle_user_value_clause(jps, type, data, data_size);
            goto out;
        }
#endif

        memcpy(&jps->key_pos, &jps->parser.pos, sizeof jps->key_pos);
        jps->key_pos.column_number -= (unsigned)(data_size + 2U); /* kludge to move the pointer back to the start of the key */
        if (data_size >= sizeof jps->cur_keyname)
            WOLFSENTRY_ERROR_OUT(CONFIG_INVALID_KEY);
        memcpy(jps->cur_keyname, data, data_size);
        jps->cur_keyname[data_size] = 0;
        jps->cur_keydepth = jps->cur_depth;
        WOLFSENTRY_RETURN_OK;
    }

    if ((type == JSON_OBJECT_BEG) || (type == JSON_ARRAY_BEG))
        ++jps->cur_depth;
    else if ((type == JSON_OBJECT_END) || (type == JSON_ARRAY_END))
        --jps->cur_depth;

    if ((type == JSON_OBJECT_BEG) && (jps->cur_depth == 1))
        WOLFSENTRY_RETURN_OK;

    if ((type == JSON_OBJECT_END) && (jps->cur_depth == 0)) {
        reset_o_u_c(jps);
        WOLFSENTRY_RETURN_OK;
    }

    if (jps->table_under_construction == T_U_C_NONE) {
        if ((jps->cur_keydepth == 1) && (jps->cur_depth <= 2))  {
            switch (type) {
            case JSON_FALSE:
            case JSON_TRUE:
            case JSON_NUMBER:
            case JSON_STRING:
                if (jps->cur_depth != 1)
                    WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);
                if (! strcmp(jps->cur_keyname, "wolfsentry-config-version")) {
                    ret = convert_uint32(type, data, data_size, &jps->config_version);
                    if (ret < 0)
                        goto out;
                    if (jps->config_version != 1)
                        WOLFSENTRY_ERROR_OUT(CONFIG_INVALID_VALUE);
                    WOLFSENTRY_RETURN_OK;
                }
                WOLFSENTRY_ERROR_OUT(CONFIG_INVALID_KEY);
            case JSON_OBJECT_BEG:
                if (jps->config_version == 0)
                    WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);
                if (jps->cur_depth != 2)
                    WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);
                if (! strcmp(jps->cur_keyname, "config-update")) {
                    jps->table_under_construction = T_U_C_TOPCONFIG;
                    WOLFSENTRY_RETURN_OK;
                }
                if (! strcmp(jps->cur_keyname, "default-policies")) {
                    jps->table_under_construction = T_U_C_DEFAULTPOLICIES;
                    WOLFSENTRY_RETURN_OK;
                }
                if (! strcmp(jps->cur_keyname, "user-values")) {
                    jps->table_under_construction = T_U_C_USER_VALUES;
                    WOLFSENTRY_RETURN_OK;
                }
                WOLFSENTRY_ERROR_OUT(CONFIG_INVALID_KEY);
            case JSON_ARRAY_BEG:
                if (jps->config_version == 0)
                    WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);
                if (jps->cur_depth != 2)
                    WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);
                if (! strcmp(jps->cur_keyname, "events-insert")) {
                    jps->table_under_construction = T_U_C_EVENTS;
                    WOLFSENTRY_RETURN_OK;
                }
                if (! strcmp(jps->cur_keyname, "static-routes-insert")) {
                    jps->table_under_construction = T_U_C_STATIC_ROUTES;
                    WOLFSENTRY_RETURN_OK;
                }
                if (! strcmp(jps->cur_keyname, "actions-update")) {
                    jps->table_under_construction = T_U_C_ACTIONS;
                    WOLFSENTRY_RETURN_OK;
                }
                WOLFSENTRY_ERROR_OUT(CONFIG_INVALID_KEY);
            case JSON_NULL:
            case JSON_KEY:
            case JSON_OBJECT_END:
            case JSON_ARRAY_END:
            WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);
            }
        }
        WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);
    }

    if (jps->table_under_construction == T_U_C_TOPCONFIG) {
        ret = handle_eventconfig_clause(jps, type, data, data_size, &jps->default_config);
        goto out;
    }

    if (jps->table_under_construction == T_U_C_DEFAULTPOLICIES) {
        ret = handle_defaultpolicy_clause(jps, type, data, data_size);
        goto out;
    }

    if (jps->table_under_construction == T_U_C_STATIC_ROUTES) {
        if ((jps->cur_depth == 1) && (type == JSON_ARRAY_END)) {
            jps->table_under_construction = T_U_C_NONE;
            WOLFSENTRY_RETURN_OK;
        }
        ret = handle_route_clause(jps, type, data, data_size);
        goto out;
    }

    if (jps->table_under_construction == T_U_C_EVENTS) {
        if ((jps->cur_depth == 1) && (type == JSON_ARRAY_END)) {
            jps->table_under_construction = T_U_C_NONE;
            WOLFSENTRY_RETURN_OK;
        }
        ret = handle_event_clause(jps, type, data, data_size);
        goto out;
    }

#if 0
    if (jps->table_under_construction == T_U_C_ACTIONS) {
        if ((jps->cur_depth == 1) && (type == JSON_ARRAY_END)) {
            jps->table_under_construction = T_U_C_NONE;
            WOLFSENTRY_RETURN_OK;
        }
        ret = handle_action_clause(jps, type, data, data_size);
        goto out;
    }
#endif

    if (jps->table_under_construction == T_U_C_USER_VALUES) {
        if ((jps->cur_depth == 1) && (type == JSON_ARRAY_END)) {
            jps->table_under_construction = T_U_C_NONE;
            WOLFSENTRY_RETURN_OK;
        }
        ret = handle_user_value_clause(jps, type, data, data_size);
        goto out;
    }

    WOLFSENTRY_ERROR_OUT(CONFIG_UNEXPECTED);

  out:

    if (ret < 0) {
        reset_o_u_c(jps);
        if (WOLFSENTRY_ERROR_CODE_IS(ret, CONFIG_INVALID_KEY))
            memcpy(&jps->parser.err_pos, &jps->key_pos, sizeof(JSON_INPUT_POS));
        else if (WOLFSENTRY_ERROR_CODE_IS(ret, CONFIG_INVALID_VALUE))
            memcpy(&jps->parser.err_pos, &jps->parser.value_pos, sizeof(JSON_INPUT_POS));
        else
            memcpy(&jps->parser.err_pos, &jps->parser.pos, sizeof(JSON_INPUT_POS));
        WOLFSENTRY_ERROR_RERETURN(ret);
    } else
        WOLFSENTRY_RETURN_OK;
}

WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_json_init_ex(
    WOLFSENTRY_CONTEXT_ARGS_IN,
    wolfsentry_config_load_flags_t load_flags,
    const JSON_CONFIG *json_config,
    struct wolfsentry_json_process_state **jps)
{
    wolfsentry_errcode_t ret;
    static const JSON_CALLBACKS json_callbacks = {
        .process = (int (*)(JSON_TYPE,  const unsigned char *, size_t,  void *))json_process
    };

    static const JSON_CONFIG default_json_config = {
        .max_total_len = 0,
        .max_total_values = 0,
        .max_number_len = 20,
        .max_string_len = WOLFSENTRY_KV_MAX_VALUE_BYTES,
        .max_key_len = WOLFSENTRY_MAX_LABEL_BYTES,
        .max_nesting_level = WOLFSENTRY_MAX_JSON_NESTING,
        .flags = JSON_NOSCALARROOT
    };

    if (json_config == NULL)
        json_config = &default_json_config;

    if (wolfsentry == NULL)
        WOLFSENTRY_ERROR_RETURN(INVALID_ARG);

    if (WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_FINI))
        WOLFSENTRY_ERROR_RETURN(INVALID_ARG);

    if (! WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_DRY_RUN|WOLFSENTRY_CONFIG_LOAD_FLAG_LOAD_THEN_COMMIT))
        WOLFSENTRY_MUTEX_OR_RETURN();
    else if (WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_DRY_RUN))
        WOLFSENTRY_SHARED_OR_RETURN();
    else
        WOLFSENTRY_PROMOTABLE_OR_RETURN();

    if ((*jps = (struct wolfsentry_json_process_state *)wolfsentry_malloc(WOLFSENTRY_CONTEXT_ARGS_OUT, sizeof **jps)) == NULL)
        WOLFSENTRY_ERROR_RETURN(SYS_RESOURCE_FAILED);
    memset(*jps, 0, sizeof **jps);
    (*jps)->load_flags = load_flags;

    if (WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_JSON_DOM_DUPKEY_ABORT)) {
#ifdef WOLFSENTRY_HAVE_JSON_DOM
        (*jps)->dom_parser_flags |= JSON_DOM_DUPKEY_ABORT;
#else
        WOLFSENTRY_ERROR_RETURN(IMPLEMENTATION_MISSING);
#endif
    }
    if (WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_JSON_DOM_DUPKEY_USEFIRST)) {
#ifdef WOLFSENTRY_HAVE_JSON_DOM
        (*jps)->dom_parser_flags |= JSON_DOM_DUPKEY_USEFIRST;
#else
        WOLFSENTRY_ERROR_RETURN(IMPLEMENTATION_MISSING);
#endif
    }
    if (WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_JSON_DOM_DUPKEY_USELAST)) {
#ifdef WOLFSENTRY_HAVE_JSON_DOM
        (*jps)->dom_parser_flags |= JSON_DOM_DUPKEY_USELAST;
#else
        WOLFSENTRY_ERROR_RETURN(IMPLEMENTATION_MISSING);
#endif
    }
    if (WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_JSON_DOM_MAINTAINDICTORDER)) {
#ifdef WOLFSENTRY_HAVE_JSON_DOM
        (*jps)->dom_parser_flags |= JSON_DOM_MAINTAINDICTORDER;
#else
        WOLFSENTRY_ERROR_RETURN(IMPLEMENTATION_MISSING);
#endif
    }

    (*jps)->wolfsentry_actual = wolfsentry;
#ifdef WOLFSENTRY_THREADSAFE
    (*jps)->thread = thread;
#endif
    if (! WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_DRY_RUN|WOLFSENTRY_CONFIG_LOAD_FLAG_LOAD_THEN_COMMIT)) {
        (*jps)->wolfsentry = wolfsentry;
    } else {
        ret = wolfsentry_context_clone(
            WOLFSENTRY_CONTEXT_ARGS_OUT,
            &(*jps)->wolfsentry,
            WOLFSENTRY_CHECK_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_FLUSH)
            ? WOLFSENTRY_CLONE_FLAG_NONE
            : WOLFSENTRY_CLONE_FLAG_AS_AT_CREATION);
        if (ret < 0)
            goto out;
        ret = wolfsentry_context_inhibit_actions(JPSP_WOLFSENTRY_CONTEXT_ARGS_OUT);
        if (ret < 0)
            goto out;
    }

    ret = json_init(WOLFSENTRY_CONTEXT_ARGS_OUT_EX(wolfsentry_get_allocator((*jps)->wolfsentry)),
                    &(*jps)->parser,
                    &json_callbacks,
                    json_config,
                    *jps);
    if (ret < 0) {
        ret = wolfsentry_centijson_errcode_translate(ret);
        goto out;
    }

    if (! WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_DRY_RUN|WOLFSENTRY_CONFIG_LOAD_FLAG_NO_FLUSH|WOLFSENTRY_CONFIG_LOAD_FLAG_LOAD_THEN_COMMIT)) {
        if ((ret = wolfsentry_context_flush(WOLFSENTRY_CONTEXT_ARGS_OUT)) < 0)
            goto out;
    }

  out:

    if (ret < 0) {
        if (WOLFSENTRY_MASKIN_BITS(load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_DRY_RUN|WOLFSENTRY_CONFIG_LOAD_FLAG_LOAD_THEN_COMMIT) &&
            ((*jps)->wolfsentry != NULL))
        {
            int ret2 = wolfsentry_context_free(JPSP_P_WOLFSENTRY_CONTEXT_ARGS_OUT);
            if (ret2 < 0)
                ret = ret2;
        }
        wolfsentry_free(WOLFSENTRY_CONTEXT_ARGS_OUT, *jps);
        *jps = NULL;
        WOLFSENTRY_ERROR_UNLOCK_AND_RERETURN(ret);
    } else
        WOLFSENTRY_RETURN_OK; /* keeping lock! */
}

WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_json_init(
    WOLFSENTRY_CONTEXT_ARGS_IN,
    wolfsentry_config_load_flags_t load_flags,
    struct wolfsentry_json_process_state **jps)
{
    WOLFSENTRY_ERROR_RERETURN(
        wolfsentry_config_json_init_ex(
            WOLFSENTRY_CONTEXT_ARGS_OUT,
            load_flags,
            NULL,
            jps));
}

/* use this to initialize configuration with nonzero route_private_data_size and
 * route_private_data_alignment, which will be loaded as the default, and also
 * subsequently be copied to any eventconfigs that are allocated for
 * events-insert configuration.
 */
WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_json_set_default_config(
    struct wolfsentry_json_process_state *jps,
    struct wolfsentry_eventconfig *config)
{
    memcpy(&jps->default_config, config, sizeof jps->default_config);
    WOLFSENTRY_RETURN_OK;
}

WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_json_feed(
    struct wolfsentry_json_process_state *jps,
    const unsigned char *json_in,
    size_t json_in_len,
    char *err_buf,
    size_t err_buf_size)
{
    JSON_INPUT_POS json_pos;

    if (WOLFSENTRY_CHECK_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_FINI))
        WOLFSENTRY_ERROR_RETURN(INVALID_ARG);
    int ret = json_feed(&jps->parser, json_in, json_in_len);
    if (ret < 0) {
        jps->fini_ret = json_fini(&jps->parser, &json_pos);
        WOLFSENTRY_SET_BITS(jps->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_FINI);
        if (err_buf) {
            if (WOLFSENTRY_ERROR_DECODE_SOURCE_ID(jps->fini_ret) == WOLFSENTRY_SOURCE_ID_UNSET)
                snprintf(err_buf, err_buf_size, "json_feed failed at offset %d, L%u, col %u, with centijson code " WOLFSENTRY_ERRCODE_FMT ": %s", (int)json_pos.offset, json_pos.line_number, json_pos.column_number, (int)jps->fini_ret, json_error_str(jps->fini_ret));
            else
                snprintf(err_buf, err_buf_size, "json_feed failed at offset %d, L%u, col %u, with " WOLFSENTRY_ERROR_FMT, (int)json_pos.offset, json_pos.line_number, json_pos.column_number, WOLFSENTRY_ERROR_FMT_ARGS(jps->fini_ret));
        }
        WOLFSENTRY_ERROR_RERETURN(wolfsentry_centijson_errcode_translate(jps->fini_ret));
    }
    WOLFSENTRY_RETURN_OK;
}

WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_centijson_errcode(struct wolfsentry_json_process_state *jps, int *json_errcode, const char **json_errmsg)
{
    if ((jps == NULL) || (jps->parser.user_data == NULL))
        WOLFSENTRY_ERROR_RETURN(INVALID_ARG);
    if (json_errcode)
        *json_errcode = jps->parser.errcode;
    if (json_errmsg)
        *json_errmsg = json_error_str(jps->parser.errcode);
    WOLFSENTRY_RETURN_OK;
}

WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_json_fini(
    struct wolfsentry_json_process_state **jps,
    char *err_buf,
    size_t err_buf_size)
{
    wolfsentry_errcode_t ret;
    JSON_INPUT_POS json_pos;

    if ((jps == NULL) || (*jps == NULL))
        WOLFSENTRY_ERROR_RETURN(INVALID_ARG);

    if (WOLFSENTRY_CHECK_BITS((*jps)->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_FINI)) {
        if ((*jps)->fini_ret < 0) {
            ret = WOLFSENTRY_ERROR_ENCODE(CONFIG_PARSER);
            goto out;
        }
    } else {
        (*jps)->fini_ret = json_fini(&(*jps)->parser, &json_pos);
        if ((*jps)->fini_ret < 0) {
            if (err_buf != NULL)
                snprintf(err_buf, err_buf_size, "json_fini failed at offset %d, L%u, col %u, with code " WOLFSENTRY_ERRCODE_FMT ": %s.",
                         (int)json_pos.offset,json_pos.line_number, json_pos.column_number, (int)(*jps)->fini_ret, json_error_str((*jps)->fini_ret));
            ret = wolfsentry_centijson_errcode_translate((*jps)->fini_ret);
            goto out;
        }
    }

    if (WOLFSENTRY_CHECK_BITS((*jps)->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_DRY_RUN)) {
        ret = WOLFSENTRY_ERROR_ENCODE(OK);
        goto out;
    }

    if (WOLFSENTRY_CHECK_BITS((*jps)->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_LOAD_THEN_COMMIT)) {
        int flush_routes_p = ! WOLFSENTRY_MASKIN_BITS((*jps)->load_flags, WOLFSENTRY_CONFIG_LOAD_FLAG_NO_FLUSH);
        struct wolfsentry_route_table *old_route_table, *new_route_table;
        if ((ret = wolfsentry_route_get_main_table(JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT, &old_route_table)) < 0)
            goto out;
        if ((ret = wolfsentry_route_get_main_table(JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT, &new_route_table)) < 0)
            goto out;
        if (wolfsentry_table_n_deletes((struct wolfsentry_table_header *)new_route_table)
            != wolfsentry_table_n_deletes((struct wolfsentry_table_header *)old_route_table))
        {
            wolfsentry_action_res_t action_results = WOLFSENTRY_ACTION_RES_NONE;
            if ((ret = wolfsentry_route_bulk_clear_insert_action_status(JPSP_WOLFSENTRY_CONTEXT_ARGS_OUT, &action_results)) < 0)
                goto out;
            flush_routes_p = 1;
        }

        ret = wolfsentry_context_exchange(JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT, (*jps)->wolfsentry);
        if (ret < 0)
            goto out;

        if (flush_routes_p) {
            if ((ret = wolfsentry_context_flush(JPSP_WOLFSENTRY_CONTEXT_ARGS_OUT)) < 0)
                goto out;
        }

        if ((ret = wolfsentry_context_enable_actions(JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT)) < 0)
            goto out;

        {
            wolfsentry_action_res_t action_results = WOLFSENTRY_ACTION_RES_NONE;
            if ((ret = wolfsentry_route_bulk_insert_actions(JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT, &action_results)) < 0)
                goto out;
        }

        WOLFSENTRY_WARN_ON_FAILURE(wolfsentry_context_free(JPSP_P_WOLFSENTRY_CONTEXT_ARGS_OUT));
    } else
        ret = WOLFSENTRY_ERROR_ENCODE(OK);

  out:

#ifdef WOLFSENTRY_HAVE_JSON_DOM
    (void)json_dom_clean(&(*jps)->dom_parser);
#endif

    if ((*jps)->wolfsentry && ((*jps)->wolfsentry != (*jps)->wolfsentry_actual))
        WOLFSENTRY_WARN_ON_FAILURE(wolfsentry_context_free(JPSP_P_WOLFSENTRY_CONTEXT_ARGS_OUT));

#ifdef WOLFSENTRY_THREADSAFE
    WOLFSENTRY_WARN_ON_FAILURE(wolfsentry_context_unlock((*jps)->wolfsentry_actual, (*jps)->thread));
#endif

    wolfsentry_free(JPSP_WOLFSENTRY_ACTUAL_CONTEXT_ARGS_OUT, *jps);

    *jps = NULL;

    if (ret < 0)
        WOLFSENTRY_ERROR_RERETURN(ret);
    else
        WOLFSENTRY_RETURN_OK;
}

WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_json_oneshot_ex(
    WOLFSENTRY_CONTEXT_ARGS_IN,
    const unsigned char *json_in,
    size_t json_in_len,
    wolfsentry_config_load_flags_t load_flags,
    const JSON_CONFIG *json_config,
    char *err_buf,
    size_t err_buf_size)
{
    wolfsentry_errcode_t ret;
    struct wolfsentry_json_process_state *jps;
    if ((ret = wolfsentry_config_json_init_ex(WOLFSENTRY_CONTEXT_ARGS_OUT, load_flags, json_config, &jps)) < 0)
        WOLFSENTRY_ERROR_RERETURN(ret);
    if ((ret = wolfsentry_config_json_feed(jps, json_in, json_in_len, err_buf, err_buf_size)) < 0) {
        ret = wolfsentry_config_json_fini(&jps, NULL, 0);
        WOLFSENTRY_ERROR_RERETURN(ret);
    }
    WOLFSENTRY_ERROR_RERETURN(wolfsentry_config_json_fini(&jps, err_buf, err_buf_size));
}

WOLFSENTRY_API wolfsentry_errcode_t wolfsentry_config_json_oneshot(
    WOLFSENTRY_CONTEXT_ARGS_IN,
    const unsigned char *json_in,
    size_t json_in_len,
    wolfsentry_config_load_flags_t load_flags,
    char *err_buf,
    size_t err_buf_size)
{
    WOLFSENTRY_ERROR_RERETURN(
        wolfsentry_config_json_oneshot_ex(
            WOLFSENTRY_CONTEXT_ARGS_OUT,
            json_in,
            json_in_len,
            load_flags,
            NULL,
            err_buf,
            err_buf_size));
}
