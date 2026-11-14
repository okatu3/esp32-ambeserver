#include "license_check.h"

#include <stdio.h>
#include <string.h>

void license_base_call(const char *call, char base[16])
{
    int n = 0;
    for (const char *p = call; *p && n < 15; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            break;                                   /* "JA1XXX/P", "JA1XXX-1", "JA1XXX D" -> JA1XXX */
        }
        base[n++] = c;
    }
    base[n] = 0;
}

bool license_is_japanese(const char *b)
{
    size_t n = strlen(b);
    if (n < 4 || n > 6 || b[2] < '0' || b[2] > '9') {
        return false;
    }
    if (b[0] == 'J' && b[1] >= 'A' && b[1] <= 'S') {
        return true;
    }
    return (b[0] == '7' || b[0] == '8') && b[1] >= 'J' && b[1] <= 'N';
}

void license_search_url(const char *base, char *url, int size)
{
    /* the search form "種類で探す" (アマチュア局, SelectOW=01) sent with GET, as the site's own form does */
    snprintf(url, (size_t)size,
             "https://www.tele.soumu.go.jp/musen/SearchServlet?SC=1&pageID=3&SelectID=1&CONFIRM=0&SelectOW=01"
             "&IT=&HC=&HV=&FF=&TF=&HZ=3&NA=&MA=%s&FC=&DFY=&DFM=&DFD=&DTY=&DTM=&DTD=&DC=100#result",
             base);
}
