/* WzMap MinGW build: minimal libxml2 stand-in. The XML stream only feeds a connected level editor (-connect);
   nodes are kept so the code paths stay valid, nothing is serialised. */
#pragma once
#include <stdlib.h>
#include <string.h>
typedef unsigned char xmlChar;
typedef struct _xmlNode { struct _xmlNode *children, *next; } xmlNode, *xmlNodePtr;
typedef struct _xmlDoc { xmlNodePtr children; } xmlDoc, *xmlDocPtr;
typedef struct _xmlBuffer { xmlChar *content; unsigned int use; } xmlBuffer, *xmlBufferPtr;
static inline xmlNodePtr xmlNewNode(void *ns, const xmlChar *name) { (void)ns; (void)name; return (xmlNodePtr)calloc(1, sizeof(xmlNode)); }
static inline xmlNodePtr xmlNewDocRawNode(xmlDocPtr d, void *ns, const xmlChar *n, const xmlChar *c) { (void)d; (void)ns; (void)n; (void)c; return (xmlNodePtr)calloc(1, sizeof(xmlNode)); }
static inline xmlDocPtr xmlNewDoc(const xmlChar *v) { (void)v; return (xmlDocPtr)calloc(1, sizeof(xmlDoc)); }
static inline xmlNodePtr xmlAddChild(xmlNodePtr p, xmlNodePtr c) { (void)p; return c; }
static inline void xmlNodeSetContent(xmlNodePtr n, const xmlChar *c) { (void)n; (void)c; }
static inline void *xmlSetProp(xmlNodePtr n, const xmlChar *a, const xmlChar *v) { (void)n; (void)a; (void)v; return 0; }
static inline xmlBufferPtr xmlBufferCreate(void) { xmlBufferPtr b = (xmlBufferPtr)calloc(1, sizeof(xmlBuffer)); b->content = (xmlChar*)calloc(1, 4); return b; }
static inline void xmlBufferFree(xmlBufferPtr b) { if (b) { free(b->content); free(b); } }
static inline int xmlNodeDump(xmlBufferPtr b, xmlDocPtr d, xmlNodePtr n, int l, int f) { (void)b; (void)d; (void)n; (void)l; (void)f; return 0; }
static inline int xmlSaveFile(const char *f, xmlDocPtr d) { (void)f; (void)d; return 0; }
