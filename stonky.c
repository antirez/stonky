/* Copyright (c) 2021, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Adding these for portablity */
#define _BSD_SOURCE
#if defined(__linux__)
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>
#include <math.h>

#include <curl/curl.h>
#include <sqlite3.h>
#include "sds.h"
#include "cJSON.h"
#include "canvas.h"
#include "kann.h"

#define C_OK 1
#define C_ERR 0
#define UNUSED(V) ((void) V)

/* Flags potentially used for multiple functions. */
#define STONKY_NOFLAGS 0        /* No special flags. */
#define STONKY_SHORT (1<<0)     /* Use short form for some output. */

int debugMode = 0; /* If true enables debugging info (--debug option). */
char *dbFile = "/tmp/stonky.sqlite"; /* Change with --dbfile. */
sqlite3 *dbHandle;
sds BotApiKey = NULL;

/* ============================================================================
 * Allocator wrapper: we want to exit on OOM instead of trying to recover.
 * ==========================================================================*/
void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (p == NULL) {
        printf("Out of memory: malloc(%zu)", size);
        exit(1);
    }
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr,size);
    if (p == NULL) {
        printf("Out of memory: realloc(%zu)", size);
        exit(1);
    }
    return p;
}

void xfree(void *ptr) {
    free(ptr);
}

/* ============================================================================
 * JSON selector implementation: cJSON is a bit too raw...
 * ==========================================================================*/

/* You can select things like this:
 *
 * cJSON *json = cJSON_Parse(myjson_string);
 * cJSON *width = cJSON_Select(json,".features.screens[*].width",4);
 * cJSON *height = cJSON_Select(json,".features.screens[4].*","height");
 * cJSON *price = cJSON_Select(json,".features.screens[4].price_*",
 *                  price_type == EUR ? "eur" : "usd");
 *
 * You can use a ":<type>" specifier, usually at the end, in order to
 * check the type of the final JSON object selected. If the type will not
 * match, the function will return NULL. For instance the specifier:
 *
 *  ".foo.bar:s"
 *
 * Will not return NULL only if the root object has a foo field, that is
 * an object with a bat field, that contains a string. This is the full
 * list of selectors:
 *
 *  ".field", select the "field" of the current object.
 *  "[1234]", select the specified index of the current array.
 *  ":<type>", check if the currently selected type is of the specified type,
 *             where the type is a single letter that can be:
 *             "s" for string
 *             "n" for number
 *             "a" for array
 *             "o" for object
 *             "b" for boolean
 *             "!" for null
 *
 * Selectors can be combined, and the special "*" can be used in order to
 * fetch array indexes or field names from the arguments:
 *
 *      cJSON *myobj = cJSON_Parse(root,".properties[*].*", index, fieldname);
 */
#define JSEL_INVALID 0
#define JSEL_OBJ 1            /* "." */
#define JSEL_ARRAY 2          /* "[" */
#define JSEL_TYPECHECK 3      /* ":" */
#define JSEL_MAX_TOKEN 256
cJSON *cJSON_Select(cJSON *o, const char *fmt, ...) {
    va_list ap;
    int next = JSEL_INVALID;        /* Type of the next selector. */
    char token[JSEL_MAX_TOKEN+1];   /* Current token. */
    int tlen;                       /* Current length of the token. */

    va_start(ap,fmt);
    const char *p = fmt;
    tlen = 0;
    while(1) {
        /* Our four special chars (plus the end of the string) signal the
         * end of the previous token and the start of the next one. */
        if (tlen && (*p == '\0' || strchr(".[]:",*p))) {
            token[tlen] = '\0';
            if (next == JSEL_INVALID) {
                goto notfound;
            } else if (next == JSEL_ARRAY) {
                if (!cJSON_IsArray(o)) goto notfound;
                int idx = atoi(token); /* cJSON API index is int. */
                if ((o = cJSON_GetArrayItem(o,idx)) == NULL)
                    goto notfound;
            } else if (next == JSEL_OBJ) {
                if (!cJSON_IsObject(o)) goto notfound;
                if ((o = cJSON_GetObjectItemCaseSensitive(o,token)) == NULL)
                    goto notfound;
            } else if (next == JSEL_TYPECHECK) {
                if (token[0] == 's' && !cJSON_IsString(o)) goto notfound;
                if (token[0] == 'n' && !cJSON_IsNumber(o)) goto notfound;
                if (token[0] == 'o' && !cJSON_IsObject(o)) goto notfound;
                if (token[0] == 'a' && !cJSON_IsArray(o)) goto notfound;
                if (token[0] == 'b' && !cJSON_IsBool(o)) goto notfound;
                if (token[0] == '!' && !cJSON_IsNull(o)) goto notfound;
            }
        } else if (next != JSEL_INVALID) {
            /* Otherwise accumulate characters in the current token, note that
             * the above check for JSEL_NEXT_INVALID prevents us from
             * accumulating at the start of the fmt string if no token was
             * yet selected. */
            if (*p != '*') {
                token[tlen] = *p++;
                tlen++;
                if (tlen > JSEL_MAX_TOKEN) goto notfound;
                continue;
            } else {
                /* The "*" character is special: if we are in the context
                 * of an array, we read an integer from the variable argument
                 * list, then concatenate it to the current string.
                 *
                 * If the context is an object, we read a string pointer
                 * from the variable argument string and concatenate the
                 * string to the current token. */
                int len;
                char buf[64];
                char *s;
                if (next == JSEL_ARRAY) {
                    int idx = va_arg(ap,int);
                    len = snprintf(buf,sizeof(buf),"%d",idx);
                    s = buf;
                } else if (next == JSEL_OBJ) {
                    s = va_arg(ap,char*);
                    len = strlen(s);
                } else {
                    goto notfound;
                }
                /* Common path. */
                if (tlen+len > JSEL_MAX_TOKEN) goto notfound;
                memcpy(token+tlen,buf,len);
                tlen += len;
                p++;
                continue;
            }
        }
        /* Select the next token type according to its type specifier. */
        if (*p == ']') p++; /* Skip closing "]", it's just useless syntax. */
        if (*p == '\0') break;
        else if (*p == '.') next = JSEL_OBJ;
        else if (*p == '[') next = JSEL_ARRAY;
        else if (*p == ':') next = JSEL_TYPECHECK;
        else goto notfound;
        tlen = 0; /* A new token starts. */
        p++; /* Token starts at next character. */
    }

cleanup:
    va_end(ap);
    return o;

notfound:
    o = NULL;
    goto cleanup;
}

/* ============================================================================
 * HTTP interface abstraction
 * ==========================================================================*/

/* The callback concatenating data arriving from CURL http requests into
 * a target SDS string. */
size_t makeHttpCallWriter(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    UNUSED(size);
    sds *body = userdata;
    *body = sdscatlen(*body,ptr,nmemb);
    return nmemb;
}

/* Request the specified URL in a blocking way, returns the content (or
 * error string) as an SDS string. If 'resptr' is not NULL, the integer
 * will be set, by referece, to C_OK or C_ERR to indicate error or success.
 * The returned SDS string must be freed by the caller both in case of
 * error and success. */
sds makeHttpCall(const char *url, int *resptr) {
    if (debugMode) printf("HTTP GET %s\n", url);
    CURL* curl;
    CURLcode res;
    sds body = sdsempty();

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, makeHttpCallWriter);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15);

        /* Perform the request, res will get the return code */
        res = curl_easy_perform(curl);
        if (resptr) *resptr = res == CURLE_OK ? C_OK : C_ERR;

        /* Check for errors */
        if (res != CURLE_OK) {
            const char *errstr = curl_easy_strerror(res);
            body = sdscat(body,errstr);
        }

        /* always cleanup */
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return body;
}

/* Like makeHttpCall(), but the list of options will be concatenated to
 * the URL as a query string, and URL encoded as needed.
 * The option list array should contain optnum*2 strings, alternating
 * option names and values. */
sds makeHttpCallOpt(const char *url, int *resptr, char **optlist, int optnum) {
    sds fullurl = sdsnew(url);
    if (optnum) fullurl = sdscatlen(fullurl,"?",1);
    CURL *curl = curl_easy_init();
    for (int j = 0; j < optnum; j++) {
        if (j > 0) fullurl = sdscatlen(fullurl,"&",1);
        fullurl = sdscat(fullurl,optlist[j*2]);
        fullurl = sdscatlen(fullurl,"=",1);
        char *escaped = curl_easy_escape(curl,
            optlist[j*2+1],strlen(optlist[j*2+1]));
        fullurl = sdscat(fullurl,escaped);
        curl_free(escaped);
    }
    curl_easy_cleanup(curl);
    sds body = makeHttpCall(fullurl,resptr);
    sdsfree(fullurl);
    return body;
}

/* Make an HTTP request to the Telegram bot API, where 'req' is the specified
 * action name. This is a low level API that is used by other bot APIs
 * in order to do higher levle work. 'resptr' works the same as in
 * makeHttpCall(). */
sds makeBotRequest(const char *action, int *resptr, char **optlist, int numopt)
{
    sds url = sdsnew("https://api.telegram.org/bot");
    url = sdscat(url,BotApiKey);
    url = sdscatlen(url,"/",1);
    url = sdscat(url,action);
    sds body = makeHttpCallOpt(url,resptr,optlist,numopt);
    sdsfree(url);
    return body;
}

/* =============================================================================
 * Yahoo financial API.
 * ===========================================================================*/

/* The following structure is returned by the getYahooData() function,
 * and is later freed by freeYahooData(). */

#define YDATA_QUOTE 0       /* Quote data. */
#define YDATA_TS 1          /* Historical time series data. */
typedef struct ydata {
    int type;       /* YDATA_QUOTE or YDATA_TS. */
    /* === Data filled for quote query type === */
    sds symbol;     /* Company stock symbol (also available for TS queries). */
    sds name;       /* Company name. */
    float pre;      /* Pre market price or zero. */
    float post;     /* Post market price or zero. */
    float reg;      /* Regular market price (also available for TS queries). */
    time_t pretime;
    time_t posttime;
    time_t regtime;
    /* Market percentage change, as a string. */
    sds prechange;
    sds postchange;
    sds regchange;
    sds exchange;   /* Exchange name. */
    int delay;      /* Data source delay. */
    sds csym;       /* Currency symbol. */
    /* === Data filled for time series queries === */
    int ts_len;         /* Number of samples. */
    float *ts_data;     /* Samples. */
    float ts_min;       /* Min sample value. */
    float ts_max;       /* Max sample value. */
} ydata;

/* Free the ydata result structure. */
void freeYahooData(ydata *y) {
    if (y == NULL) return;
    sdsfree(y->symbol);
    sdsfree(y->name);
    sdsfree(y->prechange);
    sdsfree(y->postchange);
    sdsfree(y->regchange);
    sdsfree(y->csym);
    sdsfree(y->exchange);
    free(y->ts_data);
    free(y);
}

/* Perform queries to the Yahoo API: returns NULL on error, or a structure
 * representing the obtained data on success. The returned structure should
 * be freed calling freeYahooData().
 *
 * The 'type' argument is the request type: YDATA_QUOTE or YDATA_TS, to obtain
 * quote data or time series of historical data.
 *
 * The 'range' and 'interval' arguments are both strings in the Yahoo API format
 * of a single digit followed by a one or two chars, in order to specify a time
 * interval. For instance 1s means one second, 6mo six months, and so forth.
 * Such arguments are only used for YDATA_TS queries.
 *
 * The range tells how much backlog of data is requested, while the interval
 * tells the granularity of the data. Not all combinations are valid.
 *
 * Valid ranges are: 1d, 5d, 1mo, 3mo, 6mo, 1y, 2y, 5y, 10y, ytd, max.
 *
 * Valid intervals are: 1m, 2m, 5m, 15m, 30m, 60m, 90m, 1h, 1d, 5d, 1wk,
 * 1mo, 3mo.
 *
 * Note all range and interval combinations are valid. */
ydata *getYahooData(int type, const char *symbol, const char *range, const char *interval) {
    const char *apihost = "https://query1.finance.yahoo.com";
    sds url;

    /* Build the query according to the requested data. */
    if (type == YDATA_TS) {
        url = sdsnew(apihost);
        url = sdscat(url,"/v8/finance/chart/");
        url = sdscat(url,symbol);
        url = sdscatprintf(url,"?range=%s&interval=%s&includePrePost=false",
                           range,interval);
    } else if (type == YDATA_QUOTE) {
        url = sdsnew(apihost);
        url = sdscat(url,"/v10/finance/quoteSummary/");
        url = sdscat(url,symbol);
        url = sdscat(url,"?modules=price");
    } else {
        return NULL;
    }

    /* Get data via a blocking HTTP request. */
    int res;
    sds body = makeHttpCall(url,&res);
    sdsfree(url);
    if (res != C_OK) return NULL;

    /* Setup the empty object. */
    ydata *yd = malloc(sizeof(*yd));
    memset(yd,0,sizeof(*yd));
    yd->type = type;

    /* Extract the JSON fields and fills the data object. */
    cJSON *json = cJSON_Parse(body);
    sdsfree(body);

    if (type == YDATA_QUOTE) {
        cJSON *price = cJSON_Select(json,".quoteSummary.result[0].price");
        cJSON *aux;
        if (price == NULL) goto fmterr;
        if ((aux = cJSON_Select(price,".preMarketPrice.raw:n")) != NULL)
            yd->pre = aux->valuedouble;
        if ((aux = cJSON_Select(price,".postMarketPrice.raw:n")) != NULL)
            yd->post = aux->valuedouble;
        if ((aux = cJSON_Select(price,".regularMarketPrice.raw:n")) != NULL)
            yd->reg = aux->valuedouble;
        if ((aux = cJSON_Select(price,".preMarketChangePercent.fmt:s")) != NULL)
            yd->prechange = sdsnew(aux->valuestring);
        if ((aux = cJSON_Select(price,".postMarketChangePercent.fmt:s")) != NULL)
            yd->postchange = sdsnew(aux->valuestring);
        if ((aux = cJSON_Select(price,".regularMarketChangePercent.fmt:s")) != NULL)
            yd->regchange = sdsnew(aux->valuestring);
        if ((aux = cJSON_Select(price,".preMarketTime:n")) != NULL)
            yd->pretime = aux->valuedouble;
        if ((aux = cJSON_Select(price,".postMarketTime:n")) != NULL)
            yd->posttime = aux->valuedouble;
        if ((aux = cJSON_Select(price,".regMarketTime:n")) != NULL)
            yd->regtime = aux->valuedouble;
        if ((aux = cJSON_Select(price,".symbol:s")) != NULL)
            yd->symbol = sdsnew(aux->valuestring);
        if ((aux = cJSON_Select(price,".shortName:s")) != NULL)
            yd->name = sdsnew(aux->valuestring);
        if ((aux = cJSON_Select(price,".exchangeName:s")) != NULL)
            yd->exchange = sdsnew(aux->valuestring);
        if ((aux = cJSON_Select(price,".currencySymbol:s")) != NULL)
            yd->csym = sdsnew(aux->valuestring);
        if ((aux = cJSON_Select(price,".exchangeDataDelayedBy:n")) != NULL)
            yd->delay = aux->valuedouble;
        /* Certain times tha Yahoo API is unable to return actual info
         * from a stock, even if it returns success. */
        if (yd->regchange == NULL) goto fmterr;
    } else {
        cJSON *meta = cJSON_Select(json,".chart.result[0].meta");
        cJSON *aux;
        if (meta == NULL) goto fmterr;
        if ((aux = cJSON_Select(meta,".symbol:s")) != NULL)
            yd->symbol = sdsnew(aux->valuestring);
        cJSON *data = cJSON_Select(json,
            ".chart.result[0].indicators.quote[0].close:a");
        if (data == NULL) goto fmterr;

        /* Count the items, so that we can size our timestamp array. */
        int len = 0;
        aux = data->child;
        while(aux != NULL) {
            len++;
            aux = aux->next;
        }
        yd->ts_len = len;
        yd->ts_data = malloc(sizeof(float)*len);

        /* Load data into array. */
        int idx = 0;
        aux = data->child;
        while(aux != NULL) {
            float v = aux->valuedouble;
            if (cJSON_IsNumber(aux))
                yd->ts_data[idx] = v;
            else
                yd->ts_data[idx] = 0;
            if (idx == 0 || v < yd->ts_min) yd->ts_min = v;
            if (idx == 0 || v > yd->ts_max) yd->ts_max = v;
            idx++;
            aux = aux->next;
        }
    }

    cJSON_Delete(json);
    return yd;

fmterr:
    cJSON_Delete(json);
    freeYahooData(yd);
    return NULL;
}

/* =============================================================================
 * Higher level Telegram bot API.
 * ===========================================================================*/

/* Send a message to the specified channel, optionally as a reply to a
 * specific message (if reply_to is non zero). */
int botSendMessage(int64_t target, sds text, int64_t reply_to) {
    char *options[10];
    int optlen = 4;
    options[0] = "chat_id";
    options[1] = sdsfromlonglong(target);
    options[2] = "text";
    options[3] = text;
    options[4] = "parse_mode";
    options[5] = "Markdown";
    options[6] = "disable_web_page_preview";
    options[7] = "true";
    if (reply_to) {
        optlen++;
        options[8] = "reply_to_message_id";
        options[9] = sdsfromlonglong(reply_to);
    } else {
        options[9] = NULL; /* So we can sdsfree it later without problems. */
    }

    int res;
    sds body = makeBotRequest("sendMessage",&res,options,optlen);
    sdsfree(body);
    sdsfree(options[1]);
    sdsfree(options[9]);
    return res;
}

typedef struct botRequest {
    sds request;        /* The request string. */
    int64_t target;     /* Target channel where to reply. */
} botRequest;

/* Free the bot request and associated data. */
void freeBotRequest(botRequest *br) {
    sdsfree(br->request);
    free(br);
}

/* Create a bot request object and return it to the caller. */
botRequest *createBotRequest(void) {
    botRequest *br = malloc(sizeof(*br));
    br->request = NULL;
    br->target = 0;
    return br;
}

/* =============================================================================
 * Database abstraction
 * ===========================================================================*/

/* Create the SQLite tables if needed. Return NULL on error. */
sqlite3 *dbInit(void) {
    sqlite3 *db;
    int rt = sqlite3_open(dbFile, &db);
    if (rt != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    char *sql =
    "DROP TABLE IF EXISTS Cars;"
    "CREATE TABLE IF NOT EXISTS Lists(Name TEXT);"
    "CREATE TABLE IF NOT EXISTS ListStock(listid INT, Symbol TEXT);";

    char *errmsg;
    int rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error [%d]: %s\n", rc, errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

/* Return the ID of the specified list, creating it if it does not exist.
 * If 'nocreate' is true and the list does not exist, the function just
 * returns 0. On error, zero is returned. */
int64_t dbGetListID(const char *listname, int nocreate) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    char *sql = "SELECT rowid FROM Lists WHERE name=?";
    int64_t listid = 0;

    /* Check if a list with such name already exists. */
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_text(stmt,1,listname,-1,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        listid = sqlite3_column_int64(stmt,0);
    } else if (nocreate == 0) {
        /* We need to insert it. */
        sqlite3_finalize(stmt);
        sql = "INSERT INTO Lists VALUES(?)";
        rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_text(stmt,1,listname,-1,NULL);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) goto error;
        listid = sqlite3_last_insert_rowid(dbHandle);
    }

error:
    sqlite3_finalize(stmt);
    return listid;
}

/* Remove a stock from the list, returning C_OK if the stock was
 * actually there, and was removed. Otherwise C_ERR is returned.
 * If dellist is true, and the removed stock has the effect of creating an
 * emtpy list, the list itself is removed. */
int dbDelStockFromList(const char *listname, const char *symbol, int dellist) {
    int64_t listid = dbGetListID(listname,1);
    if (listid == 0) return C_ERR;

    sqlite3_stmt *stmt = NULL;
    int rc;
    int retval = C_ERR;

    const char *sql = "DELETE FROM ListStock WHERE listid=? AND symbol=?";
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,listid);
    rc = sqlite3_bind_text(stmt,2,symbol,-1,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) goto error;
    retval = C_OK;

    if (dellist) {
        /* TODO: remove the list if this was the last stock. */
    }

error:
    sqlite3_finalize(stmt);
    return retval;
}

/* Add the stock to the specified list. Create the list if it didn't exist
 * yet. Return the stock ID in the list, or zero on error. */
int dbAddStockToList(const char *listname, const char *symbol) {
    int64_t listid = dbGetListID(listname,0);
    if (listid == 0) return 0;

    sqlite3_stmt *stmt = NULL;
    int rc;
    int64_t stockid = 0;

    /* Remove the stock from the list in case it is already there. This
     * way we avoid adding it multiple times. */
    dbDelStockFromList(listname,symbol,0);

    const char *sql = "INSERT INTO ListStock VALUES(?,?)";
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,listid);
    rc = sqlite3_bind_text(stmt,2,symbol,-1,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) goto error;
    stockid = sqlite3_last_insert_rowid(dbHandle);

error:
    sqlite3_finalize(stmt);
    return stockid;
}

/* Return the stocks in a list as an array of SDS strings and a count,
 * you can free the returned object with sdsfreesplitres().
 * If the list does not exist, zero is returned. */
sds *dbGetStocksFromList(const char *listname, int *numstocks) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    int rows = 0;
    sds *symbols = NULL;
    char *sql = "SELECT symbol FROM ListStock WHERE listid=?";

    /* Get the ID of the specified list, if any. */
    int64_t listid = dbGetListID(listname,1);
    if (listid == 0) return C_ERR;

    /* Check if a list with such name already exists. */
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,listid);
    if (rc != SQLITE_OK) goto error;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        symbols = realloc(symbols,sizeof(sds)*(rows+1));
        sds sym = sdsnew((char*)sqlite3_column_text(stmt,0));
        symbols[rows++] = sym;
    }
    *numstocks = rows;
    return symbols;

error:
    sdsfreesplitres(symbols,rows);
    sqlite3_finalize(stmt);
    return NULL;
}

/* =============================================================================
 * Bot commands implementations
 * ===========================================================================*/

/* Concat a string representing informations about a given stock price
 * to the specified SDS string.
 *
 * If flag is 0 (or STONKY_NOFLAGS), the function runs normally.
 * If flag is STONKY_SHORT, a formatting using less space is used. */
sds sdsCatPriceRequest(sds s, sds symbol, int flags) {
    ydata *yd = getYahooData(YDATA_QUOTE,symbol,NULL,NULL);
    if (yd == NULL) {
        s = sdscatprintf(s,
            (flags & STONKY_SHORT) ? "Can't find data for '%s'" :
                                     "%s: no data avaiable",
        symbol);
    } else {
        if (flags & STONKY_SHORT) {
            s = sdscatprintf(s,
                "[%s](https://google.com/search?q=%s+stock): %.02f%s (%s%s)",
                yd->symbol,
                yd->symbol,
                yd->reg,
                yd->csym,
                (yd->regchange && yd->regchange[0] == '-') ? "" : "+",
                yd->regchange);
        } else {
            s = sdscatprintf(s,
                "%s ([%s](https://google.com/search?q=%s+stock)) "
                "price is %.02f%s (%s%s)",
                yd->name,
                yd->symbol,
                yd->symbol,
                yd->reg,
                yd->csym,
                (yd->regchange && yd->regchange[0] == '-') ? "" : "+",
                yd->regchange);
        }
        if (debugMode) {
            printf("%s pretime:%d regtime:%d posttime:%d\n",
                yd->symbol,
                (int)yd->pretime, (int)yd->regtime, (int)yd->posttime);
        }
        if (yd->pretime > yd->regtime) {
            s = sdscatprintf(s," | %s: %.02f%s (%s%s)",
                (flags & STONKY_SHORT) ? "pre" : "pre-market",
                yd->pre, yd->csym, yd->prechange[0] == '-' ? "" : "+",
                yd->prechange);
        } else if (yd->posttime > yd->regtime) {
            s = sdscatprintf(s," | %s: %.02f%s (%s%s)",
                (flags & STONKY_SHORT) ? "post" : "after-hours",
                yd->post, yd->csym, yd->postchange[0] == '-' ? "" : "+",
                yd->postchange);
        }
    }
    freeYahooData(yd);
    return s;
}

/* Handle bot price requests in the form: $AAPL. */
void botHandlePriceRequest(botRequest *br, sds symbol) {
    sds reply = sdsCatPriceRequest(sdsempty(),symbol,0);
    botSendMessage(br->target,reply,0);
    sdsfree(reply);
}

/* Handle bot chart requests in the form: $AAPL 1d|5d|1m|6m|1y. */
void botHandleChartRequest(botRequest *br, sds symbol, sds range) {
    ydata *yd = NULL;
    sds reply = sdsempty();

    /* Select the Yahoo chart API parameters according to the
     * requested interval. */
    /* api_range and api_internal defaults to 1d and 5m respectively */
    char *api_range, *api_interval;
    if (!strcasecmp(range,"1d")) {
        api_range = "1d";
        api_interval = "5m";
    } else if (!strcasecmp(range,"5d")) {
        api_range = "5d";
        api_interval = "1h";
    } else if (!strcasecmp(range,"1m")) {
        api_range = "1mo";
        api_interval = "90m";
    } else if (!strcasecmp(range,"6m")) {
        api_range = "6mo";
        api_interval = "1d";
    } else if (!strcasecmp(range,"1y")) {
        api_range = "1y";
        api_interval = "5d";
    } else {
        reply = sdscatprintf(reply,"Invalid chart range. Use 1d|5d|1m|6m|1y");
        goto fmterr;
    }

    yd = getYahooData(YDATA_TS,symbol,api_range,api_interval);
    if (yd == NULL) {
        reply = sdscatprintf(reply,"Can't fetch chart data for '%s'",symbol);
    } else {
        int height = 20;
        int width = 42;
        canvas *canv = createCanvas(width,height+8,0);
        for (int j = 0; j < yd->ts_len; j++) {
            double range = yd->ts_max - yd->ts_min;
            double this = yd->ts_data[j] - yd->ts_min;
            int y = height-(this*height/range)-1;
            int x = (double)j/yd->ts_len*width;
            y += 4;
            if (j > 0) {
                drawLine(canv,x,y,x,height-1+4,1);
            }
        }
        sds graph = renderCanvas(canv);
        freeCanvas(canv);
        reply = sdscatprintf(reply,
                            "%s %s | min %.02f max %.02f\n```",
                            yd->symbol, api_range, yd->ts_min, yd->ts_max);
        reply = sdscatsds(reply,graph);
        reply = sdscat(reply,"```");
        sdsfree(graph);
    }

fmterr:
    freeYahooData(yd);
    botSendMessage(br->target,reply,0);
    sdsfree(reply);
}

/* Check if the specified time series data has more than 'maxperc' of
 * elements that are set to zero. If the zeroes are > maxperc, C_ERR is
 * returned, otherwise C_OK is returned. */
int checkYahooTimeSeriesValidity(ydata *yd, int maxperc) {
    int nulls = 0;
    for (int j = 0; j < yd->ts_len; j++)
        nulls += yd->ts_data[j] == 0;
    return (nulls > yd->ts_len/maxperc) ? C_ERR : C_OK;
}

/* Structure used by computeMonteCarlo() to return the results. */
typedef struct {
    double gain;    /* Average gain. */
    double mingain, maxgain;    /* Minimum and maximum gains. */
    double absdiff; /* Average of absolute difference of gains. */
    double avgper;  /* Average period of buy/sell action, in days. */
} mcres;

/* Perform a Montecarlo simulation where the stock is bought and sold
 * at random days within the specified 'range' interval. Return data
 * as specified in the mcres structure. */
void computeMontecarlo(ydata *yd, int range, int count, mcres *mc) {
    int buyday, sellday;
    double total_gain = 0;
    double total_interval = 0;
    float *data = yd->ts_data;

    if (range > yd->ts_len) range = yd->ts_len;
    else if (range < yd->ts_len) data += yd->ts_len-range;
    double *gains = xmalloc(sizeof(double)*count);

    for (int j = 0; j < count; j++) {
        /* We want to pick two different days, and since the API sometimes
         * return data with fields set to zero, also make sure that
         * we pick non-zero days. */
        do {
            buyday = rand() % range;
        } while (data[buyday] == 0);
        do {
            sellday = rand() % range;
        } while(sellday == buyday || data[sellday] == 0);

        if (buyday > sellday) {
            int t = buyday;
            buyday = sellday;
            sellday = t;
        }

        double buy_price = data[buyday];
        double sell_price = data[sellday];
        double gain = (sell_price-buy_price)/buy_price*100;
        gains[j] = gain;
        if (debugMode) {
            printf("buy (%d) %f sell (%d) %f: %f\n",
                buyday, buy_price, sellday, sell_price, gain);
        }
        total_gain += gain;
        total_interval += sellday-buyday;
    }
    mc->gain = total_gain / count;
    mc->avgper = total_interval / count;

    /* Scan the array of gains to calculate the average gain difference,
     * as long as min/max values. */
    mc->absdiff = 0;
    for (int j = 0; j < count; j++) {
        mc->absdiff += fabs(mc->gain - gains[j]);
        if (j == 0) {
            mc->mingain = gains[j];
            mc->maxgain = gains[j];
        } else {
            if (gains[j] < mc->mingain) mc->mingain = gains[j];
            if (gains[j] > mc->maxgain) mc->maxgain = gains[j];
        }
    }
    mc->absdiff /= count;
    xfree(gains);
}

/* Fetch 1y of data and performs a Montecarlo simulation where we buy
 * and sell at random moments, calculating the average gain (or loss). */
void botHandleMontecarloRequest(botRequest *br, sds symbol) {
    sds reply = sdsempty();

    /* Fetch the data. Sometimes we'll not obtain enough data points. */
    ydata *yd = getYahooData(YDATA_TS,symbol,"1y","1d");
    if (yd == NULL || yd->ts_len < 100) {
        reply = sdscatprintf(reply,"Can't fetch historical data for '%s'",
                             symbol);
        goto cleanup;
    }

    /* Some other times, there will be too many data points with NULL
     * inside. */
    if (checkYahooTimeSeriesValidity(yd,10) == C_ERR) {
        reply = sdscatprintf(reply,
                    "Historical data for '%s' has more than 10%% of invalid "
                    "data points. Doing the analysis would be unreliable. "
                    "Blame Yahoo Finance.", symbol);
        goto cleanup;
    }

    int count = 1000; /* Number of experiments to perform in the
                         Montecarlo simulation. */

    mcres mc;
    computeMontecarlo(yd,365,count,&mc);
    reply = sdscatprintf(reply,
        "Buying and selling '%s' at random days during "
        "the past year would result in:\n"
        "Average gain/loss: %.2f%% (+/-%.2f%%).\n"
        "Bets outcome : %.2f%%.\n"
        "Worst outcome: %.2f%%.\n"
        "%d experiments with an average interval of %.2f days.",
        symbol, mc.gain, mc.absdiff, mc.maxgain, mc.mingain, count, mc.avgper);

cleanup:
    if (reply) botSendMessage(br->target,reply,0);
    freeYahooData(yd);
    sdsfree(reply);
}

/* Handle list requests. */
void botHandleListRequest(botRequest *br, sds *argv, int argc) {
    /* Remove the final ":" from the list name. */
    sds listname = sdsnewlen(argv[0],sdslen(argv[0])-1);
    sds reply = NULL;

    if (argc == 1) {
        /* If it's just the list name, reply with a list of stocks
         * and their current price. */
        int numstocks;
        sds *stocks = dbGetStocksFromList(listname,&numstocks);
        if (stocks == NULL) {
            reply = sdscatprintf(sdsempty(),"No such list %s", listname);
        } else {
            reply = sdscatprintf(sdsempty(),"Prices for list %s:\n", listname);
            for (int j = 0; j < numstocks; j++) {
                reply = sdsCatPriceRequest(reply,stocks[j],STONKY_SHORT);
                reply = sdscat(reply,"\n");
            }
        }
        sdsfreesplitres(stocks,numstocks);
    } else {
        /* Otherwise we are in edit mode, with +... -... symbols. */
        for (int j = 1; j < argc; j++) {
            if (argv[j][0] == '+')
                dbAddStockToList(listname,argv[j]+1);
            else
                dbDelStockFromList(listname,argv[j]+1,1);
        }
        int numstocks;
        sds *stocks = dbGetStocksFromList(listname,&numstocks);
        if (stocks == NULL) {
            reply = sdscatprintf(sdsempty(),"The list %s no longer exists.",
                listname);
        } else {
            reply = sdscatprintf(sdsempty(),"The list %s is now composed of: ",
                listname);
            for (int j = 0; j < numstocks; j++) {
                reply = sdscatsds(reply,stocks[j]);
                if (j < numstocks-1) reply = sdscat(reply,", ");
            }
        }
        sdsfreesplitres(stocks,numstocks);
    }
    if (reply) botSendMessage(br->target,reply,0);
    sdsfree(reply);
    sdsfree(listname);
}

#define OUTCOME_OK 0
#define OUTCOME_NEUTRAL 1
#define OUTCOME_KO 2

/* Get the class for a given time series of prices. */
int fillNetGetClass(float *data, int ihours, int afterhours) {
    double avgprice = 0;
    for (int i = ihours; i < ihours+afterhours; i++)
        avgprice += data[i];
    avgprice /= afterhours;
    double gain = (avgprice / data[ihours-1])-1;

    int class;
    if (gain > 0.005) class = OUTCOME_OK;
    else if (gain < -0.005) class = OUTCOME_KO;
    else class = OUTCOME_NEUTRAL;
    return class;
}

/* Normalize the first 'ihours' samples of 'data' in the range from 0 to 1. */
void fillNetNormalize(float *data, int ihours) {
    float min = data[0];
    float max = data[0];
    for (int i = 0; i < ihours; i++) {
        float aux = data[i];
        if (aux > max) max = aux;
        if (aux < min) min = aux;
    }
    double norm = max-min;
    for (int i = 0; i < ihours; i++)
        data[i] = (data[i]-min) / norm;
}

/* Populate the training arrays. */
void fillNetData(float **inputs, float **outputs, int setlen, float *data, int ihours, int afterhours) {
    int numclasses = 3;
    int classhits[3] = {0};

    for (int j = 0; j < setlen; j++) {
        /* Fill this input with normalized data. */
        inputs[j] = xmalloc(sizeof(float)*ihours);
        for (int i = 0; i < ihours; i++)
            inputs[j][i] = data[j+i];
        fillNetNormalize(inputs[j],ihours);

        /* Predict class. */
        int class = fillNetGetClass(data+j,ihours,afterhours);
        classhits[class]++;

        /* Set the output class. */
        outputs[j] = xmalloc(sizeof(float)*numclasses);
        outputs[j][0] = 0;
        outputs[j][1] = 0;
        outputs[j][2] = 0;
        outputs[j][class] = 1;

#if 0
        for (int k = 0; k < ihours; k++) printf("%f,",inputs[j][k]);
        printf(" = ");
        printf("%f (gain %f | class %f %f %f),",
            avgprice,gain,
            outputs[j][0],outputs[j][1],outputs[j][2]);
        printf("\n");
#endif
    }
    printf("Trained with class hits %d %d %d\n", classhits[0],
           classhits[1], classhits[2]);
    int maxclass = classhits[0];
    if (classhits[1] > maxclass) maxclass = classhits[1];
    if (classhits[2] > maxclass) maxclass = classhits[2];
    printf("Base error: %f\n",
        (1-((double)maxclass/(classhits[0]+classhits[1]+classhits[2])))*100);
}

/* Neural network request: train a neural network to predict a given
 * stock price class in the next hours (ok, ko, neutral). */
void botHandleNeuralNetworkRequest(botRequest *br, sds symbol) {
    sds reply = sdsempty();
    float **inputs = NULL;
    float **outputs = NULL;

    /* Fetch the data. Sometimes we'll not obtain enough data points. */
    ydata *yd = getYahooData(YDATA_TS,symbol,"7d","1m");
    if (debugMode) printf("Training the net with %d samples\n",yd->ts_len);
    if (yd == NULL || yd->ts_len < 1000) {
        reply = sdscatprintf(reply,"Missing historical data for '%s'",
                             symbol);
        goto cleanup;
    }

    /* For neural network training, we don't tolerate more than 1%
     * of zero samples. */
    if (checkYahooTimeSeriesValidity(yd,1) == C_ERR) {
        reply = sdscatprintf(reply,"Too many zero samples for '%s'",
                             symbol);
        goto cleanup;
    }

    /* Create a neural network that takes N hours as input, and emits
     * the outcome of the average next hours price (ok, neutral, ko). */
    int ihours = 7*10;    /* Get as input the previous "ihours" hours. */
    int afterhours = 7*2; /* Predict average price of the next "afterhours". */
    int numclasses = 3;   /* Three prediction classes. */

    kad_node_t *t;
    kann_t *ann;

    /* Create the neural network. */
    t = kann_layer_input(ihours);
    t = kad_relu(kann_layer_dense(t, 1000));
    t = kad_relu(kann_layer_dense(t, 500));
    t = kann_layer_cost(t, numclasses, KANN_C_CEM);
    ann = kann_new(t, 0);

    /* Create the training and testing datasets. */
    int verlen = 200; /* Verify the final net using this 200 hours of data. */
    int setlen = yd->ts_len-ihours-afterhours-verlen;
    inputs = xmalloc(setlen*sizeof(float*));
    outputs = xmalloc(setlen*sizeof(float*));
    fillNetData(inputs,outputs,setlen,yd->ts_data,ihours,afterhours);

    /* Perform the training. */
    int mini_size = 64;
    int max_epoch = 100;
    int max_drop_streak = 20;
    float lr = 0.001, frac_val = 0.1;

    // kann_mt(ann, n_threads, mini_size); /* To use threads. */
    kann_train_fnn1(ann, lr, mini_size, max_epoch, max_drop_streak, frac_val,
                    setlen, inputs, outputs);

    for (int j = 0; j < setlen; j++) {
        xfree(inputs[j]);
        xfree(outputs[j]);
    }

    /* Execute the net to predict the next days price. */
    float normfactor;
    float min = yd->ts_data[yd->ts_len-1-ihours];
    float max = yd->ts_data[yd->ts_len-1-ihours];
    for (int j = 0; j < ihours; j++) {
        float aux = yd->ts_data[yd->ts_len-1-ihours+j];
        if (aux > max) max = aux;
        if (aux < min) min = aux;
    }
    normfactor = max-min;

    /* Verify the network training with never seen data. */
    float *x = xmalloc(sizeof(float)*ihours);
    const float *y;
    float *data = yd->ts_data+setlen;
    int ok_output = 0, err_output = 0;
    for (int j = 0; j < verlen; j++) {
        for (int i = 0; i < ihours; i++)
            x[i] = data[i];
        fillNetNormalize(x,ihours);
        y = kann_apply1(ann,x);

        int expected = fillNetGetClass(data,ihours,afterhours);
        int maxidx = 0;
        float maxval = y[0];
        for (int k = 0; k < numclasses; k++) {
            if (y[k] > maxval) {
                maxidx = k;
                maxval = y[k];
            }
        }
        printf("Expected class %d. Got class %d\n", expected, maxidx);
        if (expected == maxidx)
            ok_output++;
        else
            err_output++;
        data++;
    }
    printf("OK: %d, ERR: %d\n", ok_output, err_output);
    xfree(x);
    kann_delete(ann);

cleanup:
    xfree(inputs);
    xfree(outputs);
    if (reply) botSendMessage(br->target,reply,0);
    freeYahooData(yd);
    sdsfree(reply);
}

/* Request handling thread entry point. */
void *botHandleRequest(void *arg) {
    botRequest *br = arg;

    /* Parse the request as a command composed of arguments. */
    int argc;
    sds *argv = sdssplitargs(br->request,&argc);

    if (argv[0][sdslen(argv[0])-1] == ':') {
        /* :list [+... -...] */
        botHandleListRequest(br,argv,argc);
    } else if (argc == 1) {
        /* $AAPL */
        botHandlePriceRequest(br,argv[0]);
    } else if (argc == 2 && sdslen(argv[1]) == 2 &&
               isdigit(argv[1][0]) && isalpha(argv[1][1]))
    {
        /* $AAPL 5d */
        botHandleChartRequest(br,argv[0],argv[1]);
    } else if (argc == 2 && (!strcasecmp(argv[1],"mc") ||
                             !strcasecmp(argv[1],"montecarlo")))
    {
        /* $AAPL mc | montecarlo */
        botHandleMontecarloRequest(br,argv[0]);
    } else if (argc == 2 && (!strcasecmp(argv[1],"nn"))) {
        /* $AAPL nn */
        botHandleNeuralNetworkRequest(br,argv[0]);
    } else {
        botSendMessage(br->target,
            "Sorry, I can't understand your request. Try $HELP.",0);
    }

    freeBotRequest(br);
    sdsfreesplitres(argv,argc);
    return NULL;
}

/* Get the updates from the Telegram API, process them, and return the
 * ID of the highest processed update.
 *
 * The offset is the last ID already processed, the timeout is the number
 * of seconds to wait in long polling in case no request is immediately
 * available. */
int64_t botProcessUpdates(int64_t offset, int timeout) {
    char *options[6];
    int res;

    options[0] = "offset";
    options[1] = sdsfromlonglong(offset+1);
    options[2] = "timeout";
    options[3] = sdsfromlonglong(timeout);
    options[4] = "allowed_updates";
    options[5] = "message";
    sds body = makeBotRequest("getUpdates",&res,options,3);
    sdsfree(options[1]);
    sdsfree(options[3]);

    /* Parse the JSON in order to extract the message info. */
    cJSON *json = cJSON_Parse(body);
    cJSON *result = cJSON_Select(json,".result:a");
    if (result == NULL) goto fmterr;
    /* Process the array of updates. */
    cJSON *update;
    cJSON_ArrayForEach(update,result) {
        cJSON *update_id = cJSON_Select(update,".update_id:n");
        if (update_id == NULL) continue;
        int64_t thisoff = (int64_t) update_id->valuedouble;
        if (thisoff > offset) offset = thisoff;
        cJSON *chatid = cJSON_Select(update,".message.chat.id:n");
        if (chatid == NULL) continue;
        int64_t target = (int64_t) chatid->valuedouble;
        cJSON *date = cJSON_Select(update,".message.date:n");
        if (date == NULL) continue;
        time_t timestamp = date->valuedouble;
        cJSON *text = cJSON_Select(update,".message.text:s");
        if (text == NULL) continue;
        if (debugMode) printf(".message.text: %s\n", text->valuestring);

        /* Sanity check the request before starting the thread:
         * validate that is a request that is really targeting our bot. */
        char *s = text->valuestring;
        if (s[0] != '$') continue;
        if (time(NULL)-timestamp > 60*5) continue;

        /* Spawn a thread that will handle the request. */
        sds request = sdsnew(text->valuestring+1);
        botRequest *bt = createBotRequest();
        bt->request = request;
        bt->target = target;
        pthread_t tid;
        if (pthread_create(&tid,NULL,botHandleRequest,bt) != 0) {
            freeBotRequest(bt);
            continue;
        }
    }

fmterr:
    cJSON_Delete(json);
    sdsfree(body);
    return offset;
}

/* =============================================================================
 * Bot main loop
 * ===========================================================================*/

/* This is the bot main loop: we get messages using getUpdates in blocking
 * mode, but with a timeout. Then we serve requests as needed, and every
 * time we unblock, we check for completed requests (by the thread that
 * handles Yahoo Finance API calls). */
void botMain(void) {
    int64_t nextid = -100; /* Start getting the last 100 messages. */
    int previd;
    while(1) {
        previd = nextid;
        nextid = botProcessUpdates(nextid,1);
        /* We don't want to saturate all the CPU in a busy loop in case
         * the above call fails and returns immediately (for networking
         * errors for instance), so wait a bit at every cycle, but only
         * if we didn't made any progresses with the ID. */
        if (nextid == previd) usleep(100000);
    }
}

/* Check if a file named 'apikey.txt' exists, if so load the Telegram bot
 * API key from there. If the function is able to read the API key from
 * the file, as a side effect the global SDS string BotApiKey is populated. */
void readApiKeyFromFile(void) {
    FILE *fp = fopen("apikey.txt","r");
    if (fp == NULL) return;
    char buf[1024];
    if (fgets(buf,sizeof(buf),fp) == NULL) {
        fclose(fp);
        return;
    }
    buf[sizeof(buf)-1] = '\0';
    fclose(fp);
    sdsfree(BotApiKey);
    BotApiKey = sdsnew(buf);
    BotApiKey = sdstrim(BotApiKey," \t\r\n");
}

int main(int argc, char **argv) {
    srand(time(NULL));

    /* Parse options. */
    for (int j = 1; j < argc; j++) {
        int morearg = argc-j-1;
        if (!strcmp(argv[j],"--debug")) {
            debugMode = 1;
        } else if (!strcmp(argv[j],"--apikey") && morearg) {
            BotApiKey = sdsnew(argv[++j]);
        } else {
            printf("Usage: %s [--debug] [--apikey <apikey>]\n", argv[0]);
            exit(1);
        }
    }

    /* Initializations. Note that we don't redefine the SQLite allocator,
     * since SQLite errors are always handled by Stonky anyway. */
    if (BotApiKey == NULL) readApiKeyFromFile();
    if (BotApiKey == NULL) {
        printf("Provide a bot API key via --apikey or storing a file named "
               "apikey.txt in the bot working directory.\n");
        exit(1);
    }
    dbHandle = dbInit();
    if (dbHandle == NULL) exit(1);
    cJSON_Hooks jh = {.malloc_fn = xmalloc, .free_fn = xfree};
    cJSON_InitHooks(&jh);

    /* Enter the infinite loop handling the bot. */
    botMain();
    return 0;
}
