#include "hexml.h"
#include <malloc.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <stdio.h>

typedef int bool;

/////////////////////////////////////////////////////////////////////
// TYPES

int static inline end(str s) { return s.start + s.length; }

str static inline start_length(int32_t start, int32_t length)
{
    if (start < 0 || length < 0) assert(0);
    str res;
    res.start = start;
    res.length = length;
    return res;
}

str static inline start_end(int32_t start, int32_t end)
{
    return start_length(start, end - start);
}

typedef struct
{
    int size;
    int used;
    attr* attrs; // dynamically allocated buffer
} attr_buffer;


struct node
{
    str name; // tag name, e.g. <[foo]>
    str outer; // outer text, [<foo>bar</foo>]
    str inner; // inner text, <foo>[bar]</foo>
    str attrs; // all the attributes, in the attribute buffer
    str nodes; // all the nodes, in the node buffer
};

typedef struct
{
    // have a cursor at the front, which is all the stuff I have written out, final
    // have a cursor at the end, which is stack scoped, so children write out, then I do
    // when you commit, you copy over from end to front

    // nodes 
    int size;
    int used_front; // front entries, stored for good
    int used_back; // back entries, stack based, copied into front
    node* nodes; // dynamically allocated buffer
} node_buffer;


struct document
{
    char* body; // pointer to initial argument, not owned by us

    // things only used while parsing
    int cursor; // pointer to where we are in body
    int length; // total length of body
    // if cursor is >= length we have gone past the end

    char* error_message;
    node_buffer nodes;
    attr_buffer attrs;
};


/////////////////////////////////////////////////////////////////////
// RENDER CODE

typedef struct
{
    document* d;
    char* buffer;
    int length;
    int cursor;
} render;

void static inline render_char(render* r, char c)
{
    if (r->cursor < r->length)
        r->buffer[r->cursor] = c;
    r->cursor++;
}

void static inline bound(char* msg, int index, int mn, int mx)
{
    if (index < mn || index > mx)
    {
        printf("Bounds checking failed %s, got %i, which should be in %i:%i\n", msg, index, mn, mx);
        assert(0);
    }
}

void static inline bound_str(char* msg, str s, int mn, int mx)
{
    if (s.length < 0) assert(0);
    bound(msg, s.start, mn, mx);
    bound(msg, end(s), mn, mx);
}

void render_str(render* r, str s)
{
    bound_str("render_str", s, 0, r->d->length);
    render_char(r, '[');
    for (int i = 0; i < s.length; i++)
        render_char(r, r->d->body[s.start + i]);
    render_char(r, ']');
}

void render_tag(render* r, node* n);

void render_content(render* r, node* n)
{
    bound_str("render_conent inner", n->inner, 0, r->d->length);
    bound_str("render_conent nodes", n->nodes, 0, r->d->nodes.used_front);
    bound_str("render_conent attrs", n->attrs, 0, r->d->attrs.used);

    int done = n->inner.start;
    for (int i = 0; i < n->nodes.length; i++)
    {
        node* x = &r->d->nodes.nodes[n->nodes.start + i];
        render_str(r, start_end(done, x->outer.start));
        done = end(x->outer);
        render_tag(r, x);
    }
    render_str(r, start_end(done, end(n->inner)));
}

void render_tag(render* r, node* n)
{
    render_char(r, '<');
    render_str(r, n->name);
    for (int i = 0; i < n->attrs.length; i++)
    {
        attr* x = &r->d->attrs.attrs[n->attrs.start + i];
        render_char(r, ' ');
        render_str(r, x->name);
        render_char(r, '=');
        render_char(r, '\"');
        render_str(r, x->value);
        render_char(r, '\"');
    }
    render_char(r, '>');
    render_content(r, n);
    render_char(r, '<');
    render_char(r, '/');
    render_str(r, n->name);
    render_char(r, '>');
}

int document_render(document* d, char* buffer, int length)
{
    render r;
    r.d = d;
    r.buffer = buffer;
    r.length = length;
    r.cursor = 0;
    render_content(&r, document_node(d));
    return r.cursor;
}


/////////////////////////////////////////////////////////////////////
// NOT THE PARSER

char* document_error(document* d){return d->error_message;}
node* document_node(document* d){return &d->nodes.nodes[0];}
str node_inner(node* n){return n->inner;}
str node_outer(node* n){return n->outer;}

void document_free(document* d)
{
    free(d->error_message);
    free(d->nodes.nodes);
    free(d->attrs.attrs);
    free(d);
}

node* node_children(document* d, node* n, int* res)
{
    *res = n->nodes.length;
    return &d->nodes.nodes[n->nodes.start];
}

attr* node_attributes(document* d, node* n, int* res)
{
    *res = n->attrs.length;
    return &d->attrs.attrs[n->attrs.start];
}


attr* node_attributeBy(document* d, node* n, char* s, int slen)
{
    assert(0);
    return NULL;
}

// Search for given strings within a node
node* node_firstChildBy(document* d, node* n, char* s, int slen)
{
    if (slen == -1) slen = strlen(s);
    int i = n->nodes.start;
    int limit = i + n->nodes.length;
    for (; i < limit; i++)
    {
        if (d->nodes.nodes[i].name.length == slen &&
            memcmp(s, &d->body[d->nodes.nodes[i].name.start], slen) == 0)
            return &d->nodes.nodes[i];
    }
    return NULL;
}

node* node_nextChildBy(document* d, node* parent, node* prev, char* s, int slen)
{
    assert(0);
    return NULL;
}


/////////////////////////////////////////////////////////////////////
// PARSER COMBINATORS

char static inline peekAt(document* d, int i)
{
    int j = d->cursor + i;
    return j >= d->length ? 0 : d->body[j];
}

void static inline skip(document* d, int i){d->cursor += i;}
char static inline peek(document* d){return peekAt(d, 0);}
char static inline get(document* d){char c = peek(d); skip(d, 1); return c;}

bool static inline is_space(char c){ return c == ' ' || c == '\t' || c == '\r' || c == '\n';}

// Remove whitespace characters from the cursor while they are still whitespace
void static inline trim(document* d)
{
    while (isspace(peek(d)))
        skip(d, 1);
}

// Find this character form the cursor onwards, if true adjust the cursor to that char, otherwise leave it at the end
bool find(document* d, char c)
{
    char* start = &d->body[d->cursor];
    char* end = memchr(start, c, d->length - d->cursor);
    if (end == NULL)
    {
        d->cursor = d->length;
        return 0;
    }
    else
    {
        d->cursor += end - start;
        return 1;
    }
}


/////////////////////////////////////////////////////////////////////
// PARSING CODE

void node_alloc(node_buffer* b, int ask)
{
    int space = b->size - b->used_back - b->used_front;
    if (space >= ask) return;
    assert(0);
}

void attr_alloc(attr_buffer* b, int ask)
{
    int space = b->size - b->used;
    if (space >= ask) return;
    int size2 = (b->size + 1000 + ask) * 2;
    attr* buf2 = malloc(size2 * sizeof(attr));
    memcpy(buf2, b->attrs, b->used * sizeof(attr));
    free(b->attrs);
    b->attrs = buf2;
}

int static inline isName(char c)
{
    return
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-';
}

// you now expect a name, perhaps preceeded by whitespace
// the name may be empty
str parse_name(document* d)
{
    trim(d);
    str res;
    res.start = d->cursor;
    while (isName(peek(d)))
        get(d);
    res.length = d->cursor - res.start;
    return res;
}

str parse_attrval(document* d)
{
    trim(d);
    if (peek(d) != '=') return start_length(0,0);
    skip(d, 1);
    trim(d);
    char c = peek(d);
    if (c == '\"' || c == '\'')
    {
        skip(d, 1);
        int start = d->cursor;
        if (!find(d, c))
        {
            d->error_message = _strdup("Couldn't find closing attribute bit");
            return start_length(0, 0);
        }
        skip(d, 1);
        return start_end(start, d->cursor - 1);
    }
    else
        return parse_name(d);
}



// seen a tag name, now looking for attributes terminated by >
// puts the attributes it finds in the attribute buffer
str parse_attributes(document* d)
{
    str res;
    res.start = d->attrs.used;
    for (int i = 0; ; i++)
    {
        str name = parse_name(d);
        if (name.length == 0) break;
        attr_alloc(&d->attrs, 1);
        d->attrs.attrs[d->attrs.used].name = name;
        d->attrs.attrs[d->attrs.used].value = parse_attrval(d);
        d->attrs.used++;
    }
    res.length = d->attrs.used - res.start;
    return res;
}

str parse_content(document* d);


// Add a new entry into tag, am at a '<'
void parse_tag(document* d)
{
    node_alloc(&d->nodes, 1);
    d->nodes.used_back++;
    int me = d->nodes.size - d->nodes.used_back;

    d->nodes.nodes[me].outer.start = d->cursor;
    char c = get(d);
    assert(c == '<');
    if (peek(d) == '?') skip(d, 1);
    d->nodes.nodes[me].name = parse_name(d);
    d->nodes.nodes[me].attrs = parse_attributes(d);

    c = get(d);
    if ((c == '/' || c == '?') && peek(d) == '>')
    {
        skip(d, 1);
        d->nodes.nodes[me].nodes = start_length(0, 0);
        d->nodes.nodes[me].outer.length = start_end(d->nodes.nodes[me].outer.start, d->cursor).length;
        d->nodes.nodes[me].inner = start_length(d->cursor, 0);
        return;
    }
    else if (c != '>')
    {
        d->error_message = _strdup("Gunk at the end of the tag");
        return;
    }
    d->nodes.nodes[me].inner.start = d->cursor;
    d->nodes.nodes[me].nodes = parse_content(d);
    d->nodes.nodes[me].inner.length = start_end(d->nodes.nodes[me].inner.start, d->cursor).length;

    if (d->error_message != NULL) return;
    if (peek(d) == '<' && peekAt(d, 1) == '/')
    {
        skip(d, 2);
        trim(d);
        if (d->length - d->cursor >= d->nodes.nodes[me].name.length &&
            memcmp(&d->body[d->cursor], &d->body[d->nodes.nodes[me].name.start], d->nodes.nodes[me].name.length) == 0)
        {
            skip(d, d->nodes.nodes[me].name.length);
            trim(d);
            if (get(d) == '>')
            {
                d->nodes.nodes[me].outer.length = start_end(d->nodes.nodes[me].outer.start, d->cursor).length;
                return;
            }
        }
        d->error_message = _strdup("Mismatch in closing tags");
        return;
    }
    d->error_message = _strdup("Weirdness when trying to close tags");
}

// Parser until </, return the index of your node children
str parse_content(document* d)
{
    int before = d->nodes.used_back;
    while (d->error_message == NULL)
    {
        // only < can have any impact
        if (!find(d, '<'))
        {
            break;
        }
        if (peekAt(d, 1) == '/')
        {
            // have found a </
            break;
        }
        else
        {
            parse_tag(d);
        }
    }
    int diff = d->nodes.used_back - before;
    node_alloc(&d->nodes, diff);
    str res = start_length(d->nodes.used_front, diff);
    for (int i = 0; i < diff; i++)
        d->nodes.nodes[d->nodes.used_front + i] = d->nodes.nodes[d->nodes.size - d->nodes.used_back + diff - 1 - i];
    d->nodes.used_front += diff;
    d->nodes.used_back -= diff;
    return res;
}

document* document_parse(char* s, int slen)
{
    if (slen == -1) slen = strlen(s);

    document* d = malloc(sizeof(document));
    d->body = s;
    d->cursor = 0;
    d->length = slen;
    d->error_message = NULL;
    d->attrs.size = 0;
    d->attrs.used = 0;
    d->attrs.attrs = NULL;
    d->nodes.size = 1000;
    d->nodes.used_back = 0;
    d->nodes.used_front = 1;
    d->nodes.nodes = malloc(sizeof(node) * 1000);

    d->nodes.nodes[0].name = start_length(0, 0);
    d->nodes.nodes[0].outer = start_length(0, slen);
    d->nodes.nodes[0].inner = start_length(0, slen);
    d->nodes.nodes[0].attrs = start_length(0, 0);
    d->nodes.nodes[0].nodes = parse_content(d);

    if (d->cursor < d->length && d->error_message == NULL)
    {
        d->error_message = _strdup("Trailing junk at the end of the document");
    }
    return d;
}
