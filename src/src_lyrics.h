#ifndef SOURCE_LYRICS
#define SOURCE_LYRICS

#include "config.h"
#include "mpdclient.h"

#include <sys/types.h>
#include <glib.h>
#include <gmodule.h>

typedef struct _formed_text
{
        GString *text;
        GArray *lines;
        int val;
} formed_text;

void formed_text_init(formed_text *text);
void add_text_line(formed_text *dest, const char *src, int len);

typedef struct _retrieval_spec
{
        mpdclient_t *client;
        int way;
} retrieval_spec;

GTimer *dltime;
short int lock;
formed_text lyr_text;

guint8 result;

/* result is a bitset in which the success when searching 4 lyrics is logged
countend by position - backwards
0: lyrics in database
1: proper access  to the lyrics provider
2: lyrics found
3: exact match
4: lyrics downloaded
5: lyrics saved
wasting 3 bits doesn't mean being a fat memory hog like kde.... does it?
*/


typedef struct src_lyr src_lyr;

struct src_lyr
{
  char *name;
  char *source_name;
  char *description;
  
  int (*register_src_lyr) (src_lyr *source_descriptor);
  int (*deregister_src_lyr)(void);

  int (*check_lyr) (char *artist, char *title, char *url);
  int (*get_lyr) (char *artist, char *title);
  int (*state_lyr)(void);

#ifndef DISABLE_PLUGIN_SYSTEM
  GModule *module;
#endif
};

typedef int (*src_lyr_plugin_register) (src_lyr *source_descriptor);

GArray *src_lyr_stack;

int get_text_line(formed_text *text, unsigned num, char *dest, size_t len);

void src_lyr_stack_init(void);
int src_lyr_init(void);
int get_lyr_by_src (int priority, char *artist, char *title);

#endif
