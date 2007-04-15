#ifndef _PURPLE_PERL_COMMON_H_
#define _PURPLE_PERL_COMMON_H_

#include <glib.h>
#ifdef _WIN32
#undef pipe
#endif
#include <XSUB.h>
#include <EXTERN.h>
#include <perl.h>

#include "plugin.h"
#include "value.h"

#define is_hvref(o) \
	((o) && SvROK(o) && SvRV(o) && (SvTYPE(SvRV(o)) == SVt_PVHV))

#define hvref(o) \
	(is_hvref(o) ? (HV *)SvRV(o) : NULL);

#define PURPLE_PERL_BOOT_PROTO(x) \
	void boot_Purple__##x(pTHX_ CV *cv);

#define PURPLE_PERL_BOOT(x) \
	purple_perl_callXS(boot_Purple__##x, cv, mark)

typedef struct
{
	PurplePlugin *plugin;
	char *package;
	char *load_sub;
	char *unload_sub;
	char *prefs_sub;
#ifdef PURPLE_GTKPERL
	char *gtk_prefs_sub;
#endif
	char *plugin_action_sub;
} PurplePerlScript;

void purple_perl_normalize_script_name(char *name);

SV *newSVGChar(const char *str);

void purple_perl_callXS(void (*subaddr)(pTHX_ CV *cv), CV *cv, SV **mark);
void purple_perl_bless_plain(const char *stash, void *object);
SV *purple_perl_bless_object(void *object, const char *stash);
gboolean purple_perl_is_ref_object(SV *o);
void *purple_perl_ref_object(SV *o);

int execute_perl(const char *function, int argc, char **args);

#if 0
gboolean purple_perl_value_from_sv(PurpleValue *value, SV *sv);
SV *purple_perl_sv_from_value(const PurpleValue *value);
#endif

void *purple_perl_data_from_sv(PurpleValue *value, SV *sv);
SV *purple_perl_sv_from_vargs(const PurpleValue *value, va_list *args,
                            void ***copy_arg);

#endif /* _PURPLE_PERL_COMMON_H_ */
