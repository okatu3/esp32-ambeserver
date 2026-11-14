#pragma once
/* Licence check by eye: builds the address of the public search of the Ministry of Internal Affairs and
 * Communications (総務省 電波利用ポータル 無線局等情報検索, amateur stations, 呼出符号 = the callsign) so the
 * program can open it in the user's own browser. The program itself sends nothing to that site. */
#include <stdbool.h>

/* the callsign without /P, -1 and the like, upper case */
void license_base_call(const char *callsign, char base[16]);
/* Japanese amateur prefixes JA-JS, 7J-7N, 8J-8N followed by a digit, 4 to 6 characters */
bool license_is_japanese(const char *base);
/* https://www.tele.soumu.go.jp/musen/SearchServlet?...&MA=<base>... */
void license_search_url(const char *base, char *url, int size);
