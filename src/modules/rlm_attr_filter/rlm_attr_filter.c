/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License, version 2 if the
 *   License as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
 
/**
 * $Id$
 * @file rlm_attr_filter.c
 * @brief Filter the contents of a list, allowing only certain attributes.
 * 
 * @copyright (C) 2001,2006 The FreeRADIUS server project
 * @copyright (C) 2001 Chris Parker <cparker@starnetusa.net>
 */
#include	<freeradius-devel/ident.h>
RCSID("$Id$")

#include	<freeradius-devel/radiusd.h>
#include	<freeradius-devel/modules.h>
#include	<freeradius-devel/rad_assert.h>

#include	<sys/stat.h>

#include	<ctype.h>
#include	<fcntl.h>
#include        <limits.h>


/*
 *	Define a structure with the module configuration, so it can
 *	be used as the instance handle.
 */
typedef struct rlm_attr_filter {
	char		*file;
	char		*key;
	int		relaxed;
	PAIR_LIST	*attrs;
} rlm_attr_filter_t;

static const CONF_PARSER module_config[] = {
	{ "file",     PW_TYPE_FILENAME,
	  offsetof(rlm_attr_filter_t, file), NULL, "${raddbdir}/attrs" },
	{ "key",     PW_TYPE_STRING_PTR,
	  offsetof(rlm_attr_filter_t, key), NULL, "%{Realm}" },
	{ "relaxed",    PW_TYPE_BOOLEAN,
	  offsetof(rlm_attr_filter_t, relaxed), NULL, "no" },
	{ NULL, -1, 0, NULL, NULL }
};

static void check_pair(VALUE_PAIR *check_item, VALUE_PAIR *reply_item,
                      int *pass, int *fail)
{
	int compare;

	if (check_item->op == T_OP_SET) return;

	compare = paircmp(check_item, reply_item);
	if (compare == 1) {
		++*(pass);
	} else {
		++*(fail);
	}

	return;
}


static int attr_filter_getfile(const char *filename, PAIR_LIST **pair_list)
{
	int rcode;
	PAIR_LIST *attrs = NULL;
	PAIR_LIST *entry;
	VALUE_PAIR *vp;

	rcode = pairlist_read(filename, &attrs, 1);
	if (rcode < 0) {
		return -1;
	}

	/*
	 * Walk through the 'attrs' file list.
	 */

	entry = attrs;
	while (entry) {

		entry->check = entry->reply;
		entry->reply = NULL;

		for (vp = entry->check; vp != NULL; vp = vp->next) {

		    /*
		     * If it's NOT a vendor attribute,
		     * and it's NOT a wire protocol
		     * and we ignore Fall-Through,
		     * then bitch about it, giving a good warning message.
		     */
		     if ((vp->da->vendor == 0) &&
			 (vp->da->attr > 0xff) &&
			 (vp->da->attr > 1000)) {
			log_debug("[%s]:%d WARNING! Check item \"%s\"\n"
				  "\tfound in filter list for realm \"%s\".\n",
				  filename, entry->lineno, vp->da->name,
				  entry->name);
		    }
		}

		entry = entry->next;
	}

	*pair_list = attrs;
	return 0;
}


/*
 *	Clean up.
 */
static int attr_filter_detach(void *instance)
{
	rlm_attr_filter_t *inst = instance;
	pairlist_free(&inst->attrs);

	return 0;
}


/*
 *	(Re-)read the "attrs" file into memory.
 */
static int attr_filter_instantiate(CONF_SECTION *conf, void **instance)
{
	rlm_attr_filter_t *inst;
	int rcode;

	*instance = inst = talloc_zero(conf, rlm_attr_filter_t);
	if (!inst) {
		return -1;
	}

	if (cf_section_parse(conf, inst, module_config) < 0) {
		return -1;
	}

	rcode = attr_filter_getfile(inst->file, &inst->attrs);
        if (rcode != 0) {
		radlog(L_ERR, "Errors reading %s", inst->file);

		return -1;
	}

	return 0;
}


/*
 *	Common attr_filter checks
 */
static rlm_rcode_t attr_filter_common(void *instance, REQUEST *request,
			      	      RADIUS_PACKET *packet)
{
	rlm_attr_filter_t *inst = instance;
	VALUE_PAIR	*vp;
	VALUE_PAIR	*output;
	VALUE_PAIR	**output_tail;
	VALUE_PAIR	*check_item;
	PAIR_LIST	*pl;
	int		found = 0;
	int		pass, fail = 0;
	char		*keyname = NULL;
	VALUE_PAIR	**input;
	char		buffer[256];

	if (!packet) return RLM_MODULE_NOOP;

	input = &(packet->vps);

	if (!inst->key) {
		VALUE_PAIR	*namepair;

		namepair = pairfind(request->packet->vps, PW_REALM, 0, TAG_ANY);
		if (!namepair) {
			return (RLM_MODULE_NOOP);
		}
		keyname = namepair->vp_strvalue;
	} else {
		int len;

		len = radius_xlat(buffer, sizeof(buffer), inst->key,
				  request, NULL, NULL);
		if (!len) {
			return RLM_MODULE_NOOP;
		}
		keyname = buffer;
	}

	output = NULL;
	output_tail = &output;

	/*
	 *      Find the attr_filter profile entry for the entry.
	 */
	for (pl = inst->attrs; pl; pl = pl->next) {
		int fall_through = 0;
		int relax_filter = inst->relaxed;

		/*
		 *  If the current entry is NOT a default,
		 *  AND the realm does NOT match the current entry,
		 *  then skip to the next entry.
		 */
		if ((strcmp(pl->name, "DEFAULT") != 0) &&
		    (strcmp(keyname, pl->name) != 0))  {
		    continue;
		}

		RDEBUG2("Matched entry %s at line %d", pl->name,
		       pl->lineno);
		found = 1;

		for (check_item = pl->check;
			check_item != NULL;
			check_item = check_item->next) {
			if (!check_item->da->vendor &&
			    (check_item->da->attr == PW_FALL_THROUGH) &&
				(check_item->vp_integer == 1)) {
				fall_through = 1;
				continue;
			}
			else if (!check_item->da->vendor &&
				 check_item->da->attr == PW_RELAX_FILTER) {
				relax_filter = check_item->vp_integer;
				continue;
			}

			/*
			 *    If it is a SET operator, add the attribute to
			 *    the output list without checking it.
			 */
			if (check_item->op == T_OP_SET ) {
				vp = paircopyvp(check_item);
				if (!vp) {
					pairfree(&output);
					return RLM_MODULE_FAIL;
				}
				radius_xlat_move(request, output_tail, &vp);
				output_tail = &((*output_tail)->next);
			}
		}

		/*
		 *	Iterate through the input items, comparing
		 *	each item to every rule, then moving it to the
		 *	output list only if it matches all rules
		 *	for that attribute.  IE, Idle-Timeout is moved
		 *	only if it matches all rules that describe an
		 *	Idle-Timeout.
		 */
		for (vp = *input; vp != NULL; vp = vp->next ) {
			/* reset the pass,fail vars for each reply item */
			pass = fail = 0;

			/*
			 *	reset the check_item pointer to
			 *	beginning of the list
			 */
			for (check_item = pl->check;
			     check_item != NULL;
			     check_item = check_item->next) {
				/*
				 *	Vendor-Specific is special, and
				 *	matches any VSA if the comparison
				 *	is always true.
				 */
				if ((check_item->da->attr == PW_VENDOR_SPECIFIC) &&
					(vp->da->vendor != 0) &&
					(check_item->op == T_OP_CMP_TRUE)) {
					pass++;
					continue;
				}

				if (vp->da->attr == check_item->da->attr) {
					check_pair(check_item, vp,
						   &pass, &fail);
				}
			}

			/*  
			 *  Only move attribute if it passed all rules,
			 *  or if the config says we should copy unmatched
			 *  attributes ('relaxed' mode).
			 */
			if (fail == 0 && (pass > 0 || relax_filter)) {
				if (!pass) {
					RDEBUG3("Attribute (%s) allowed by relaxed mode", vp->da->name);
				}
				*output_tail = paircopyvp(vp);
				if (!*output_tail) {
					pairfree(&output);
					return RLM_MODULE_FAIL;
				}
				output_tail = &((*output_tail)->next);
			}
		}

		/* If we shouldn't fall through, break */
		if (!fall_through)
			break;
	}

	/*
	 *	No entry matched.  We didn't do anything.
	 */
	if (!found) {
		rad_assert(output == NULL);
		return RLM_MODULE_NOOP;
	}

	pairfree(input);
	*input = output;

	if (request->packet->code == PW_AUTHENTICATION_REQUEST) {
		request->username = pairfind(request->packet->vps, PW_STRIPPED_USER_NAME, 0, TAG_ANY);
		if (!request->username) 
			request->username = pairfind(request->packet->vps, PW_USER_NAME, 0, TAG_ANY);
		request->password = pairfind(request->packet->vps, PW_USER_PASSWORD, 0, TAG_ANY);
	}

	return RLM_MODULE_UPDATED;
}

static rlm_rcode_t attr_filter_preacct(void *instance, REQUEST *request)
{
	return attr_filter_common(instance, request, request->packet);
}

static rlm_rcode_t attr_filter_accounting(void *instance, REQUEST *request)
{
	return attr_filter_common(instance, request, request->reply);
}

#ifdef WITH_PROXY
static rlm_rcode_t attr_filter_preproxy(void *instance, REQUEST *request)
{
	return attr_filter_common(instance, request, request->proxy);
}

static rlm_rcode_t attr_filter_postproxy(void *instance, REQUEST *request)
{
	return attr_filter_common(instance, request, request->proxy_reply);
}
#endif

static rlm_rcode_t attr_filter_postauth(void *instance, REQUEST *request)
{
	return attr_filter_common(instance, request, request->reply);
}

static rlm_rcode_t attr_filter_authorize(void *instance, REQUEST *request)
{
	return attr_filter_common(instance, request, request->packet);
}


/* globally exported name */
module_t rlm_attr_filter = {
	RLM_MODULE_INIT,
	"attr_filter",
	RLM_TYPE_CHECK_CONFIG_SAFE | RLM_TYPE_HUP_SAFE,   	/* type */
	attr_filter_instantiate,	/* instantiation */
	attr_filter_detach,		/* detach */
	{
		NULL,			/* authentication */
		attr_filter_authorize,	/* authorization */
		attr_filter_preacct,	/* pre-acct */
		attr_filter_accounting,	/* accounting */
		NULL,			/* checksimul */
#ifdef WITH_PROXY
		attr_filter_preproxy,	/* pre-proxy */
		attr_filter_postproxy,	/* post-proxy */
#else
		NULL, NULL,
#endif
		attr_filter_postauth	/* post-auth */
	},
};

