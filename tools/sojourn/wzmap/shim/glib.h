/* WzMap MinGW build: the few glib pieces q3map2's vfs / inout use */
#pragma once
#include <stdlib.h>
#include <string.h>
typedef int gboolean;
typedef char gchar;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef struct _GSList { void *data; struct _GSList *next; } GSList;
static inline GSList *g_slist_append(GSList *l, void *d) {
	GSList *n = (GSList*)calloc(1, sizeof(GSList)); n->data = d;
	if (!l) return n; GSList *t = l; while (t->next) t = t->next; t->next = n; return l; }
static inline GSList *g_slist_remove(GSList *l, const void *d) {
	GSList *p = NULL, *t = l; while (t && t->data != d) { p = t; t = t->next; }
	if (!t) return l; if (p) p->next = t->next; else l = t->next; free(t); return l; }
#define g_slist_next(l) ((l) ? (l)->next : NULL)
static inline gchar *g_locale_to_utf8(const gchar *s, long len, void *a, void *b, void *c) { (void)len; (void)a; (void)b; (void)c; return strdup(s); }
static inline void g_free(void *p) { free(p); }
static inline gchar *g_strdup(const gchar *s) { return s ? strdup(s) : NULL; }
typedef unsigned int guint32;
static inline gchar *g_strdown(gchar *s) { for (gchar *p = s; p && *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }
