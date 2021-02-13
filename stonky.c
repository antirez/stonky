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

/* Note that C_OK is 1 and C_ERR is 0, so if functions return success
 * it is possible to do things like: if (function()) {... do on success ...} */
#define C_OK 1
#define C_ERR 0
#define UNUSED(V) ((void) V)

/* Flags potentially used for multiple functions. */
#define STONKY_NOFLAGS 0        /* No special flags. */
#define STONKY_SHORT (1<<0)     /* Use short form for some output. */

int debugMode = 0; /* If true enables debugging info (--debug option). */
int verboseMode = 0; /* If true enables verbose info (--verbose && --debug) */
char *dbFile = "/tmp/stonky.sqlite";    /* Change with --dbfile. */
_Thread_local sqlite3 *dbHandle = NULL; /* Per-thread sqlite handle. */
sds BotApiKey = NULL;
sds *Symbols; /* Global list of symbols loaded from marketdata/symbols.txt */
int NumSymbols; /* Number of elements in Symbols. */

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
        } else {
            /* Return C_ERR if the request worked but returned a 500 code. */
            long code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            if (code == 500 && resptr) *resptr = C_ERR;
        }

        /* always cleanup */
        curl_easy_cleanup(curl);
    }
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

    /* Get data via a blocking HTTP request. We try to perform more than
     * a single query since sometimes the Yahoo API will return a 500
     * error without any reason, but will work again immediately after. */
    int attempt = 0, maxattempts = 5;
    int res = C_ERR;
    sds body = NULL;
    while(attempt < maxattempts && res == C_ERR) {
        if (attempt > 0) usleep(100000);
        sdsfree(body);
        body = makeHttpCall(url,&res);
        attempt++;
    }
    sdsfree(url);
    if (res != C_OK) {
        sdsfree(body);
        return NULL;
    }

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

/* Create the SQLite tables if needed (if createdb is true), and return
 * the SQLite database handle. Return NULL on error. */
sqlite3 *dbInit(int createdb) {
    sqlite3 *db;
    int rt = sqlite3_open(dbFile, &db);
    if (rt != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    if (createdb) {
        char *sql =
    "CREATE TABLE IF NOT EXISTS Lists(name TEXT COLLATE NOCASE);"
    "CREATE INDEX IF NOT EXISTS idx_lists_name ON Lists(name);"

    "CREATE TABLE IF NOT EXISTS ListStock(listid INT, "
                                          "symbol TEXT COLLATE NOCASE);"
    "CREATE INDEX IF NOT EXISTS idx_liststock_listid ON ListStock(listid);"
    "CREATE INDEX IF NOT EXISTS idx_liststock_ls ON ListStock(listid,symbol);"

    "CREATE TABLE IF NOT EXISTS StockPack(liststockid INT, "
                                          "quantity INT, "
                                          "avgprice REAL);"
    "CREATE INDEX IF NOT EXISTS idx_stockpack_lsid ON StockPack(liststockid);"
    ;

        char *errmsg;
        int rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "SQL error [%d]: %s\n", rc, errmsg);
            sqlite3_free(errmsg);
            sqlite3_close(db);
            return NULL;
        }
    }
    return db;
}

/* Should be called every time a thread exists, so that if the thread has
 * an SQLite thread-local handle, it gets closed. */
void dbClose(void) {
    if (dbHandle) sqlite3_close(dbHandle);
    dbHandle = NULL;
}

/* Return the ID of the specified list.
 * If 'create' is true and the list does not exist, the function creates it
 * and returns the ID of the newly created list.
 * On error, zero is returned. */
int64_t dbGetListID(const char *listname, int create) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    char *sql = "SELECT rowid FROM Lists WHERE name=? COLLATE NOCASE";
    int64_t listid = 0;

    /* Check if a list with such name already exists. */
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_text(stmt,1,listname,-1,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        listid = sqlite3_column_int64(stmt,0);
    } else if (create) {
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

/* Return the ID of the specified stock in the specified list.
 * The function returns 0 if the stock is not part of the list or if the
 * list does not exist at all. */
int64_t dbGetStockID(const char *listname, const char *stock) {
    int64_t listid = dbGetListID(listname,0);
    if (listid == 0) return 0;

    sqlite3_stmt *stmt = NULL;
    int rc;
    char *sql = "SELECT rowid FROM ListStock WHERE listid=? AND symbol=? "
                 "COLLATE NOCASE";
    int64_t stockid = 0;

    /* Check if a list with such name already exists. */
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,listid);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_text(stmt,2,stock,-1,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        stockid = sqlite3_column_int64(stmt,0);
    } else {
        stockid = 0;
    }

error:
    sqlite3_finalize(stmt);
    return stockid;
}

/* Remove a stock from the list, returning C_OK if the stock was
 * actually there, and was removed. Otherwise C_ERR is returned.
 * If dellist is true, and the removed stock has the effect of creating an
 * emtpy list, the list itself is removed. */
int dbDelStockFromList(const char *listname, const char *symbol, int dellist) {
    int64_t listid = dbGetListID(listname,0);
    if (listid == 0) return C_ERR;

    sqlite3_stmt *stmt = NULL;
    int rc;
    int retval = C_ERR;

    const char *sql = "DELETE FROM ListStock WHERE listid=? AND symbol=? "
                      "COLLATE NOCASE";
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,listid);
    if (rc != SQLITE_OK) goto error;
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

/* This represents buyed stocks associated with a symbol in a given list.
 * It is used by multiple functions in order to manipulate portfolios. */
typedef struct stockpack {
    int64_t rowid;
    int64_t stockid;
    int quantity;
    double avgprice;
    /* Only filled by dbGetPortfolio. */
    char symbol[64];
    double gain, gainperc;
    double daygain, daygainperc;
    double value; /* Total value of this stock at the current price. */
} stockpack;

/* Populate the fields of the stockpack that can be calculated fetching
 * info from Yahoo. On success C_OK is returned, otherwise C_ERR. */
int populateStockPackInfo(stockpack *pack, const char *symbol) {
    ydata *yd = getYahooData(YDATA_QUOTE,symbol,NULL,NULL);
    if (yd) {
        double daychange = strtod(yd->regchange,NULL);
        double payed = pack->quantity * pack->avgprice;
        double value = pack->quantity * yd->reg;
        pack->value = value;
        pack->gain = value-payed;
        pack->gainperc = (value/payed-1)*100;
        pack->daygainperc = daychange;
        pack->daygain = value / 100 * daychange;
        freeYahooData(yd);
        return C_OK;
    } else {
        return C_ERR;
    }
}

/* Return the list of portfolio stocks associated with the specified
 * list name, as an array of '*count' stockpack items. The caller
 * must be free the returned value with xfree().
 *
 * If there is no such list, or no stockpack at all associated with the
 * list, NULL is returned. */
stockpack *dbGetPortfolio(const char *listname, int *count) {
    int64_t listid = dbGetListID(listname,0);
    if (listid == 0) return NULL;

    stockpack *packs = NULL;
    int rows = 0;
    sqlite3_stmt *stmt = NULL;
    int rc;
    char *sql = "SELECT symbol,quantity,avgprice FROM ListStock "
                "CROSS JOIN StockPack On "
                "ListStock.rowid = StockPack.liststockid "
                "WHERE ListStock.listid=? ORDER BY symbol";

    /* Check if a list with such name already exists. */
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,listid);
    if (rc != SQLITE_OK) goto error;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        packs = realloc(packs,sizeof(stockpack)*(rows+1));
        stockpack *pack = packs+rows;
        memset(pack,0,sizeof(stockpack));
        char *symbol = (char*)sqlite3_column_text(stmt,0);
        size_t len = strlen((char*)symbol);
        if (len >= sizeof(pack->symbol)) len = sizeof(packs->symbol)-1;
        memcpy(pack->symbol,symbol,len);
        pack->symbol[len] = 0;
        pack->quantity = sqlite3_column_int64(stmt,1);
        pack->avgprice = sqlite3_column_double(stmt,2);
        /* Compute the gain. */
        populateStockPackInfo(pack,symbol);
        rows++;
    }
    *count = rows;

    /* Return NULL if the list exists but there are no associated packs. */
    if (rows == 0) {
        xfree(packs);
        packs = NULL;
    }

error:
    sqlite3_finalize(stmt);
    return packs;
}

/* Add the stock to the specified list. Create the list if it didn't exist
 * yet. Return the stock ID in the list, or zero on error. */
int64_t dbAddStockToList(const char *listname, const char *symbol) {
    int64_t listid = dbGetListID(listname,1);
    if (listid == 0) return 0;

    /* Check if the stock is already part of the list. */
    int64_t stockid = dbGetStockID(listname,symbol);
    if (stockid) return stockid;

    int rc;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO ListStock VALUES(?,?)";
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,listid);
    if (rc != SQLITE_OK) goto error;
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
 * If the list does not exist, NULL is returned. */
sds *dbGetStocksFromList(const char *listname, int *numstocks) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    int rows = 0;
    sds *symbols = NULL;
    char *sql = "SELECT symbol FROM ListStock WHERE listid=?";

    /* Get the ID of the specified list, if any. */
    int64_t listid = dbGetListID(listname,0);
    if (listid == 0) return NULL;

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
    sqlite3_finalize(stmt);
    return symbols;

error:
    sdsfreesplitres(symbols,rows);
    sqlite3_finalize(stmt);
    return NULL;
}

/* Fetch the stockpack for the stockid in sp->stockid.
 * If a stockpack is found C_OK is returned, and the 'sp' structure gets
 * filled by reference. Otherwise if no stockpack was found, or in case
 * of query errors, C_ERR is returned. */
int dbGetStockPack(stockpack *sp) {
    sqlite3_stmt *stmt = NULL;
    int retval = C_ERR, rc;
    char *sql = "SELECT rowid,quantity,avgprice FROM "
                "StockPack WHERE liststockid=?";

    /* Check if a list with such name already exists. */
    rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
    if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(stmt,1,sp->stockid);
    if (rc != SQLITE_OK) goto error;
    if (sqlite3_step(stmt) != SQLITE_ROW) goto error;
    sp->rowid = sqlite3_column_int64(stmt,0);
    sp->quantity = sqlite3_column_int64(stmt,1);
    sp->avgprice = sqlite3_column_double(stmt,2);
    retval = C_OK;

error:
    sqlite3_finalize(stmt);
    return retval;
}

/* Update the stockpack according to its description in the 'sp'
 * structure passed by pointer. If 'rowid' is 0, then we want a new
 * stockpack with the specified quantity and stock ID to be added, otherwise
 * the function will just update the old stockpack.
 *
 * If a new stockpack is created, rowid is populated with its ID.
 *
 * If sp->quantity is zero, the stockpack is deleted from the DB. */
int dbUpdateStockPack(stockpack *sp) {
    sqlite3_stmt *stmt = NULL;
    int retval = C_ERR, rc;

    if (sp->rowid == 0) {
        char *sql = "INSERT INTO StockPack VALUES(?,?,?)";
        rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_int64(stmt,1,sp->stockid);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_int64(stmt,2,sp->quantity);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_double(stmt,3,sp->avgprice);
        if (rc != SQLITE_OK) goto error;
        if (sqlite3_step(stmt) != SQLITE_DONE) goto error;
        sp->rowid = sqlite3_last_insert_rowid(dbHandle);
    } else if (sp->quantity > 0) {
        char *sql = "UPDATE StockPack SET quantity=?,avgprice=? "
                    "WHERE rowid=?";
        rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_int64(stmt,1,sp->quantity);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_double(stmt,2,sp->avgprice);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_int64(stmt,3,sp->rowid);
        if (rc != SQLITE_OK) goto error;
        if (sqlite3_step(stmt) != SQLITE_DONE) goto error;
    } else {
        char *sql = "DELETE StockPack WHERE rowid=?";
        rc = sqlite3_prepare_v2(dbHandle,sql,-1,&stmt,NULL);
        if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_int64(stmt,1,sp->quantity);
        if (rc != SQLITE_OK) goto error;
        if (sqlite3_step(stmt) != SQLITE_DONE) goto error;
    }
    retval = C_OK;

error:
    sqlite3_finalize(stmt);
    return retval;
}


/* Add a new stockpack for the specified symbol and into the specified list.
 * Note that if a stock pack already exists for this stocks, the bot will
 * merge the new pack with the old one, calculating the average price.
 *
 * If no price is given, the current price will be used.
 * if the list does not exist it will be created.
 * If the symbol is not yet in the list, it will be added.
 *
 * The new stockpack total quantity and amount is returned by
 * filling 'spp' if not NULL.
 *
 * On success C_OK is returned, on error C_ERR. */
int dbBuyStocks(const char *listname, const char *symbol, double price, int quantity, stockpack *spp) {
    /* Create the list and its stock. */
    int64_t listid = dbGetListID(listname,1);
    if (listid == 0) return C_ERR;
    int stockid = dbAddStockToList(listname,symbol);
    if (stockid == 0) return C_ERR;

    /* Check if a pack exists for this stock, in order to adjust
     * quantity and price. */
    stockpack sp;
    sp.rowid = 0;
    sp.stockid = stockid;
    if (dbGetStockPack(&sp)) {
        double totprice = (sp.avgprice * sp.quantity) + (price * quantity);
        sp.quantity += quantity;
        sp.avgprice = totprice / sp.quantity;
    } else {
        sp.quantity = quantity;
        sp.avgprice = price;
    }
    int retval = dbUpdateStockPack(&sp);
    if (spp && retval == C_OK) *spp = sp;
    return retval;
}

/* Remove the specified amount of stocks from the specified stockpack associated
 * with the specified list. If quantity is 0 it means remove all the stocks.
 * On success C_OK is returned, on error (or if the list does not exit)
 * C_ERR is returned.
 *
 * If you ask to sell more stocks than the ones you have in the pack,
 * then the operation is not performed, C_ERR is returned, and this is signaled
 * by setting *ssp->quantity to a negative number.
 *
 * On success the *ssp structure is filled with the condition of the stock
 * *after* the selling. */
int dbSellStocks(const char *listname, const char *symbol, int quantity, stockpack *sp) {
    sp->quantity = 0; /* Whatever happens, don't return a negative value
                         if not on purpose. */

    /* Lookup the stock ID in that list. */
    int64_t stockid = dbGetStockID(listname,symbol);
    if (stockid == 0) {
        sp->quantity = -quantity;
        return C_ERR;
    }

    /* Fetch the stock. */
    sp->stockid = stockid;
    if (dbGetStockPack(sp) == C_ERR) {
        sp->quantity = -quantity;
        return C_ERR;
    }
    if (quantity == 0) quantity = sp->quantity;

    /* Finally update. */
    sp->quantity -= quantity;
    if (sp->quantity < 0) return C_ERR; /* Not enough stocks. */
    return dbUpdateStockPack(sp);
}

/* =============================================================================
 * Bot commands implementations
 * ===========================================================================*/

/* Concat a string representing informations about a given stock price
 * to the specified SDS string.
 *
 * If flag is 0 (or STONKY_NOFLAGS), the function runs normally.
 * If flag is STONKY_SHORT, a formatting using less space is used. */
sds sdsCatPriceRequest(sds s, sds symbol, ydata *yd, int flags) {
    if (yd == NULL) {
        s = sdscatprintf(s,
            (flags & STONKY_SHORT) ? "Can't find data for '%s'" :
                                     "%s: no data avaiable",
        symbol);
        return s;
    }

    /* Select the emoji according to the price change. */
    double change = strtod(yd->regchange,NULL);
    int emoidx = 0;
    char *emoset[] = {"⚰️","🔴","🟢","🚀"};
    /* Note: ordering of the followign if statements is important. */
    if (change < 0) emoidx = 1;
    if (change < -8) emoidx = 0;
    if (change >= 0) emoidx = 2;
    if (change > 8) emoidx = 3;

    if (flags & STONKY_SHORT) {
        s = sdscatprintf(s,
            "%s[%s](https://google.com/search?q=%s+stock): %.02f%s (%s%s)",
            emoset[emoidx],
            yd->symbol,
            yd->symbol,
            yd->reg,
            yd->csym,
            (yd->regchange && yd->regchange[0] == '-') ? "" : "+",
            yd->regchange);
    } else {
        s = sdscatprintf(s,
            "%s%s ([%s](https://google.com/search?q=%s+stock)) "
            "price is %.02f%s (%s%s)",
            emoset[emoidx],
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
    return s;
}

/* Handle bot price requests in the form: $AAPL. */
void botHandlePriceRequest(botRequest *br, sds symbol) {
    ydata *yd = getYahooData(YDATA_QUOTE,symbol,NULL,NULL);
    sds reply = sdsCatPriceRequest(sdsempty(),symbol,yd,0);
    freeYahooData(yd);
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
    double absdiffper; /* Percentage of absdiff compared to gain. */
    double avgper;  /* Average period of buy/sell action, in days. */
} mcres;

/* Perform a Montecarlo simulation where the stock is bought and sold
 * at random days within the specified:
 *
 *  range: last N days where to perform the experiment.
 *  count: number of experiments.
 *  period: number of days between experiments, if 0 it is random.
 *
 * The result is stored in the mcres structure passed by reference. */
void computeMontecarlo(ydata *yd, int range, int count, int period, mcres *mc) {
    int buyday, sellday;
    double total_gain = 0;
    double total_interval = 0;
    float *data = yd->ts_data;

    if (range > yd->ts_len) range = yd->ts_len;
    else if (range < yd->ts_len) data += yd->ts_len-range;
    double *gains = xmalloc(sizeof(double)*count);

    /* The period (days of difference) between buy and sell must always
     * be less than the range (total days considered). If we have a range
     * of 2 days, the maximum period must be 1 in order to buy and
     * sell in the only two days that are 1 day apart. */
    if (period >= range) period = range-1;

    for (int j = 0; j < count; j++) {
        /* We want to pick two different days, and since the API sometimes
         * return data with fields set to zero, also make sure that
         * we pick non-zero days. */
        int maxtries; /* To avoid infinite loops when picking days. */
        if (period == 0) {
            /* Random days. */
            maxtries = 10;
            do {
                buyday = rand() % range;
            } while (data[buyday] == 0 && maxtries--);

            maxtries = 10;
            do {
                sellday = rand() % range;
            } while((sellday == buyday || data[sellday] == 0) &&
                    maxtries--);

            if (buyday > sellday) {
                int t = buyday;
                buyday = sellday;
                sellday = t;
            }
        } else {
            /* Fixed days interval. Pick the first days at random, then
             * use the offset to pick the other. Note that period < range
             * since we checked at the function entry. */
            maxtries = 10;
            do {
                buyday = rand() % (range-period);
            } while (data[buyday] == 0 && maxtries--);
            sellday = buyday+period;
        }

        double buy_price = data[buyday];
        double sell_price = data[sellday];
        double gain = (sell_price-buy_price)/buy_price*100;
        gains[j] = gain;
        if (debugMode) {
            printf("Montecarlo buy (%d) %f sell (%d) %f: %f\n",
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
    mc->absdiffper = mc->absdiff/fabs(mc->gain)*100;
    xfree(gains);
}

/* Fetch 1y of data and performs a Montecarlo simulation where we buy
 * and sell at random moments, calculating the average gain (or loss). */
void botHandleMontecarloRequest(botRequest *br, sds symbol, sds *argv, int argc) {
    sds reply = sdsempty();
    int period = 0;
    int range = 250; /* Markets are open a bit more than 250 days per year. */
    ydata *yd = NULL;

    /* Parse arguments. */
    for (int j = 0; j < argc; j++) {
        int moreargs = argc-j-1;
        if (!strcasecmp(argv[j],"period") && moreargs) {
            period = atoi(argv[++j]);
            if (period <= 0) period = 1;
        } else if (!strcasecmp(argv[j],"range") && moreargs) {
            range = atoi(argv[++j]);
            if (range <= 0) range = 1;
            if (range > 2) {
                /* Adjust for the amount of open market days in 1 year. */
                range = (double)range*5/7.2;
            }
        } else if (!strcasecmp(argv[j],"help")) {
            reply = sdsnew(
                "`$SYMBOL mc [period <days>] [range <days>]`\n"
                "A period of 0 (default) means to use a random period "
                "between buy and sell. The default range is 365 days.");
            goto cleanup;
        }
    }

    /* Fetch the data. Sometimes we'll not obtain enough data points. */
    yd = getYahooData(YDATA_TS,symbol,"5y","1d");
    if (yd == NULL || yd->ts_len < range) {
        reply = sdscatprintf(reply,
            "Can't fetch historical data for '%s', use the range option to "
            "limit the amount of history to analyze.",
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
    computeMontecarlo(yd,range,count,period,&mc);
    reply = sdscatprintf(reply,
        "Random buying/selling '%s' simulation report:\n"
        "Average gain/loss: %.2f%% (+/-%.2f%%).\n"
        "Best outcome : %.2f%%.\n"
        "Worst outcome: %.2f%%.\n"
        "%d experiments within %d days (adjusted) range using %s "
        "interval of %.2f days.",
        symbol, mc.gain, mc.absdiff, mc.maxgain, mc.mingain, count, range,
        period ? "a fixed" : "an average",
        mc.avgper);

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
                ydata *yd = getYahooData(YDATA_QUOTE,stocks[j],NULL,NULL);
                reply = sdsCatPriceRequest(reply,stocks[j],yd,STONKY_SHORT);
                freeYahooData(yd);
                reply = sdscat(reply,"\n");
            }
        }
        sdsfreesplitres(stocks,numstocks);
    } else if (!strcasecmp(argv[1],"buy") && (argc == 3 || argc == 4)) {
        /* $list: buy SYMBOL [100@price] */
        int quantity = 1;
        double price = 0;
        sds symbol = argv[2];

        /* Parse the quantity@price argument if available. */
        if (argc == 4) {
            sds details = sdsdup(argv[3]);
            char *p = strchr(details,'@');
            if (p) price = strtod(p+1,NULL);
            quantity = atoi(details);
            sdsfree(details);
        }

        /* Check that the symbol exists. */
        ydata *yd = getYahooData(YDATA_QUOTE,symbol,NULL,NULL);
        if (yd == NULL) {
            reply = sdscatprintf(sdsempty(),
                "Stock symbol %s not found", symbol);
            goto fmterr;
        }
        if (price == 0) price = yd->reg;
        freeYahooData(yd);

        /* Ready to materialize the buy operation on the DB. */
        stockpack sp;
        if (dbBuyStocks(listname,symbol,price,quantity,&sp) == C_ERR) {
            reply = sdsnew("Error adding the stock pack");
        } else {
            reply = sdscatprintf(sdsempty(),
                "Now you have %d %s stocks at an average price of %.2f",
                sp.quantity,symbol,sp.avgprice);
        }
    } else if (!strcasecmp(argv[1],"sell") && (argc == 3 || argc == 4)) {
        /* $list: sell [quantity] */
        int quantity = 0; /* All the stocks. */
        sds symbol = argv[2];

        /* Parse the quantity argument if available. */
        if (argc == 4) quantity = atoi(argv[3]);

        /* Check that the symbol exists. */
        ydata *yd = getYahooData(YDATA_QUOTE,symbol,NULL,NULL);
        if (yd == NULL) {
            reply = sdscatprintf(sdsempty(),
                "Stock symbol %s not found", symbol);
            goto fmterr;
        }
        freeYahooData(yd);

        stockpack sp;
        if (dbSellStocks(listname,symbol,quantity,&sp) == C_ERR) {
            if (sp.quantity < 0) {
                reply = sdsnew("You don't have enough stocks to sell");
            } else {
                reply = sdsnew("Error removing from the stock pack");
            }
        } else {
            reply = sdscatprintf(sdsempty(),
                "You are left with %d %s stocks at an average price of %.2f",
                sp.quantity,symbol,sp.avgprice);
        }
    } else {
        /* Otherwise we are in edit mode, with +... -... symbols. */
        for (int j = 1; j < argc; j++) {
            if (argv[j][0] == '+') {
                /* +SYMBOL */
                dbAddStockToList(listname,argv[j]+1);
            } else if (argv[j][0] == '-') {
                /* -SYMBOL. */
                dbDelStockFromList(listname,argv[j]+1,1);
            } else if (argv[j][0] == '?') {
                /* Do nothing, we want just the list of symbols. */
            } else {
                reply = sdsnew("Syntax error: use +AAPL -TWTR and so forth");
                goto fmterr;
            }
        }

        /* Show the new composition of the list. */
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

fmterr:
    if (reply) botSendMessage(br->target,reply,0);
    sdsfree(reply);
    sdsfree(listname);
}

/* Handle show portfolio requests. */
void botHandleShowPortfolioRequest(botRequest *br, sds *argv) {
    /* Remove the final "?" from the list name. */
    sds listname = sdsnewlen(argv[0],sdslen(argv[0])-1);
    sds reply = NULL;
    int count;
    stockpack *packs = dbGetPortfolio(listname,&count);
    if (packs == NULL) {
        reply = sdscatprintf(sdsempty(),
            "There aren't buyed stocks associated with the list %s",listname);
        goto cleanup;
    }

    /* Build the reply composed of all the stocks. */
    reply = sdsnew("```\n");
    for (int j = 0; j < count; j++) {
        stockpack *pack = packs+j;
        sds emoji = sdsempty();

        /* One symbol for every 10% of change. */
        double gp = fabs(pack->gainperc);
        do {
            emoji = sdscat(emoji, (pack->gainperc >= 0) ? "💚":"💔");
            gp -= 10;
        } while(gp >= 10);

        reply = sdscatprintf(reply,"%-5s | %-3d | %s%.2f (%s%.2f%%) %s\n",
            pack->symbol,
            pack->quantity,
            (pack->gain >= 0) ? "+" : "",
            pack->gain,
            (pack->gainperc >= 0) ? "+" : "",
            pack->gainperc,
            emoji);
        sdsfree(emoji);
    }
    reply = sdscat(reply,"```");

cleanup:
    xfree(packs);
    if (reply) botSendMessage(br->target,reply,0);
    sdsfree(reply);
    sdsfree(listname);
}

/* Request handling thread entry point. */
void *botHandleRequest(void *arg) {
    dbHandle = dbInit(0);
    botRequest *br = arg;

    /* Parse the request as a command composed of arguments. */
    int argc;
    sds *argv = sdssplitargs(br->request,&argc);

    if (argv[0][sdslen(argv[0])-1] == ':') {
        /* $list: [+... -...] */
        botHandleListRequest(br,argv,argc);
    } else if (argv[0][sdslen(argv[0])-1] == '?') {
        /* $list? */
        botHandleShowPortfolioRequest(br,argv);
    } else if (argc == 1) {
        /* $AAPL */
        botHandlePriceRequest(br,argv[0]);
    } else if (argc == 2 && sdslen(argv[1]) == 2 &&
               isdigit(argv[1][0]) && isalpha(argv[1][1]))
    {
        /* $AAPL 5d */
        botHandleChartRequest(br,argv[0],argv[1]);
    } else if (argc >= 2 && (!strcasecmp(argv[1],"mc") ||
                             !strcasecmp(argv[1],"montecarlo")))
    {
        /* $AAPL mc | montecarlo [options] */
        botHandleMontecarloRequest(br,argv[0],argv+1,argc-1);
    } else {
        botSendMessage(br->target,
            "Sorry, I can't understand your request. Try $HELP.",0);
    }

    freeBotRequest(br);
    sdsfreesplitres(argv,argc);
    dbClose();
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
        if (verboseMode)
            printf("Starting thread to serve: \"%s\"\n",bt->request);
    }

fmterr:
    cJSON_Delete(json);
    sdsfree(body);
    return offset;
}

/* =============================================================================
 * Periodic scanning of symbols looking for stocks having dramatic changes
 * ===========================================================================*/

/* Load symbols in the global Symbols array, also populating NumSymbols.
 * If the file can't be loaded C_ERR is returned. On success the function
 * returns C_OK. */
int loadSymbols(void) {
    if (Symbols != NULL) return C_ERR; /* Already loaded. */
    FILE *fp = fopen("marketdata/symbols.txt","r");
    if (fp == NULL) return C_ERR;

    char buf[1024];
    while (fgets(buf,sizeof(buf),fp) != NULL) {
        buf[sizeof(buf)-1] = '\0';
        for (unsigned int i = 0; buf[i] && i < sizeof(buf); i++) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                buf[i] = 0;
                break;
            }
        }
        Symbols = xrealloc(Symbols,sizeof(sds)*(NumSymbols+1));
        Symbols[NumSymbols++] = sdsnew(buf);
    }
    fclose(fp);

    /* Shuffle them: since we don't store the symbols in a database,
     * we want to start scanning them in different order at every
     * restart. */
    for (int j = 0; j < NumSymbols; j++) {
        int with = rand() % NumSymbols;
        sds aux = Symbols[j];
        Symbols[j] = Symbols[with];
        Symbols[with] = aux;
    }

    if (verboseMode) printf("%d Symbols loaded\n", NumSymbols);
    return C_OK;
}

/* This thread continuously scan stocks looking for ones that have certain
 * special features, and putting them into lists. */
void *scanStocksThread(void *arg) {
    UNUSED(arg);
    dbHandle = dbInit(0);
    int j = 0;

    while(1) {
        sds symbol = Symbols[j % NumSymbols];
        j++;

        if (debugMode) printf("Background scanning %s\n", symbol);

        /* Fetch 5y of data. Abort if we have less than 250 prices. */
        ydata *yd = getYahooData(YDATA_TS,symbol,"5y","1d");
        if (yd == NULL || yd->ts_len < 250) {
            freeYahooData(yd);
            continue;
        }

        /* Don't handle data with more than 1% of bogus entries. */
        if (checkYahooTimeSeriesValidity(yd,1) == C_ERR) {
            freeYahooData(yd);
            continue;
        }

        /* Compute Montecarlo two times, for the last year, and for
         * the last two months, detecting big changes. */
        mcres mclong, mcshort, mcvs, mcday;
        computeMontecarlo(yd,250,1000,5,&mclong);
        computeMontecarlo(yd,50,1000,5,&mcshort);
        computeMontecarlo(yd,20,1000,5,&mcvs);
        computeMontecarlo(yd,10,1000,1,&mcday);
        freeYahooData(yd);

        if (mclong.gain < mcshort.gain &&
            mcshort.gain <  mcvs.gain &&
            mcvs.gain > 8 &&
            mclong.gain < 5 &&
            mcday.gain > 1 &&
            mcday.absdiffper < 100)
        {
            if (verboseMode) {
                printf("%d/%d %s:\n"
                       "  %f (+-%f) [%f/%f] ->\n"
                       "  %f (+-%f) [%f/%f] ->\n"
                       "  %f (+-%f) [%f/%f]\n"
                       "D %f (+-%f %f%%) [%f/%f]\n",
                    j,NumSymbols,symbol,
                    mclong.gain, mclong.absdiff, mclong.mingain,
                    mclong.maxgain, mcshort.gain, mcshort.absdiff,
                    mcshort.mingain, mcshort.maxgain, mcvs.gain,
                    mcvs.absdiff, mcvs.mingain, mcvs.maxgain, mcday.gain,
                    mcday.absdiff, mcday.absdiffper, mcday.mingain,
                    mcday.maxgain);
            }
            dbAddStockToList("tothemoon", symbol);
        } else {
            dbDelStockFromList("tothemoon", symbol, 0);
        }
        sleep(1);
    }
    dbClose();
    return NULL;
}

/* Start background threads continuously doing certain tasks. */
void startBackgroundTasks(void) {
    pthread_t tid;
    if (pthread_create(&tid,NULL,scanStocksThread,NULL) != 0) {
        printf("Can't create the thread to scan stocks on the background\n");
        exit(1);
    }
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
            verboseMode = 1;
        } else if (!strcmp(argv[j],"--verbose")) {
            verboseMode = 1;
        } else if (!strcmp(argv[j],"--apikey") && morearg) {
            BotApiKey = sdsnew(argv[++j]);
        } else {
            printf(
            "Usage: %s [--apikey <apikey>] [--debug] [--verbose]\n",argv[0]);
            exit(1);
        }
    }

    /* Initializations. Note that we don't redefine the SQLite allocator,
     * since SQLite errors are always handled by Stonky anyway. */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (BotApiKey == NULL) readApiKeyFromFile();
    if (BotApiKey == NULL) {
        printf("Provide a bot API key via --apikey or storing a file named "
               "apikey.txt in the bot working directory.\n");
        exit(1);
    }
    dbHandle = dbInit(1);
    if (dbHandle == NULL) exit(1);
    cJSON_Hooks jh = {.malloc_fn = xmalloc, .free_fn = xfree};
    cJSON_InitHooks(&jh);
    loadSymbols();
    if (Symbols) startBackgroundTasks();

    /* Enter the infinite loop handling the bot. */
    botMain();
    return 0;
}
