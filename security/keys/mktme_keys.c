// SPDX-License-Identifier: GPL-3.0

/* Documentation/x86/mktme/ */

#include <linux/init.h>
#include <linux/key.h>
#include <linux/key-type.h>
#include <linux/mm.h>
#include <linux/parser.h>
#include <linux/string.h>
#include <asm/intel_pconfig.h>
#include <keys/mktme-type.h>
#include <keys/user-type.h>

#include "internal.h"

static DEFINE_SPINLOCK(mktme_lock);
static unsigned int mktme_available_keyids;  /* Free Hardware KeyIDs */

enum mktme_keyid_state {
	KEYID_AVAILABLE,	/* Available to be assigned */
	KEYID_ASSIGNED,		/* Assigned to a userspace key */
	KEYID_REF_KILLED,	/* Userspace key has been destroyed */
	KEYID_REF_RELEASED,	/* Last reference is released */
};

/* 1:1 Mapping between Userspace Keys (struct key) and Hardware KeyIDs */
struct mktme_mapping {
	struct key		*key;
	enum mktme_keyid_state	state;
};

static struct mktme_mapping *mktme_map;

int mktme_reserve_keyid(struct key *key)
{
	int i;

	if (!mktme_available_keyids)
		return 0;

	for (i = 1; i <= mktme_nr_keyids(); i++) {
		if (mktme_map[i].state == KEYID_AVAILABLE) {
			mktme_map[i].state = KEYID_ASSIGNED;
			mktme_map[i].key = key;
			mktme_available_keyids--;
			return i;
		}
	}
	return 0;
}

enum mktme_opt_id {
	OPT_ERROR,
	OPT_TYPE,
	OPT_ALGORITHM,
};

static const match_table_t mktme_token = {
	{OPT_TYPE, "type=%s"},
	{OPT_ALGORITHM, "algorithm=%s"},
	{OPT_ERROR, NULL}
};

/* Key Service Method to create a new key. Payload is preparsed. */
int mktme_instantiate_key(struct key *key, struct key_preparsed_payload *prep)
{
	unsigned long flags;
	int keyid;

	spin_lock_irqsave(&mktme_lock, flags);
	keyid = mktme_reserve_keyid(key);
	spin_unlock_irqrestore(&mktme_lock, flags);
	if (!keyid)
		return -ENOKEY;
	return 0;
}

/* Make sure arguments are correct for the TYPE of key requested */
static int mktme_check_options(u32 *payload, unsigned long token_mask,
			       enum mktme_type type, enum mktme_alg alg)
{
	if (!token_mask)
		return -EINVAL;

	switch (type) {
	case MKTME_TYPE_CPU:
		if (test_bit(OPT_ALGORITHM, &token_mask))
			*payload |= (1 << alg) << 8;
		else
			return -EINVAL;

		*payload |= MKTME_KEYID_SET_KEY_RANDOM;
		break;

	case MKTME_TYPE_NO_ENCRYPT:
		*payload |= MKTME_KEYID_NO_ENCRYPT;
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

/* Parse the options and store the key programming data in the payload. */
static int mktme_get_options(char *options, u32 *payload)
{
	enum mktme_alg alg = MKTME_ALG_AES_XTS_128;
	enum mktme_type type = MKTME_TYPE_ERROR;
	substring_t args[MAX_OPT_ARGS];
	unsigned long token_mask = 0;
	char *p = options;
	int token;

	while ((p = strsep(&options, " \t"))) {
		if (*p == '\0' || *p == ' ' || *p == '\t')
			continue;
		token = match_token(p, mktme_token, args);
		if (token == OPT_ERROR)
			return -EINVAL;
		if (test_and_set_bit(token, &token_mask))
			return -EINVAL;

		switch (token) {
		case OPT_TYPE:
			type = match_string(mktme_type_names,
					    ARRAY_SIZE(mktme_type_names),
					    args[0].from);
			if (type < 0)
				return -EINVAL;
			break;

		case OPT_ALGORITHM:
			/* Algorithm must be generally supported */
			alg = match_string(mktme_alg_names,
					   ARRAY_SIZE(mktme_alg_names),
					   args[0].from);
			if (alg < 0)
				return -EINVAL;

			/* Algorithm must be activated on this platform */
			if (!(mktme_algs & (1 << alg)))
				return -EINVAL;
			break;

		default:
			return -EINVAL;
		}
	}
	return mktme_check_options(payload, token_mask, type, alg);
}

void mktme_free_preparsed_payload(struct key_preparsed_payload *prep)
{
	kzfree(prep->payload.data[0]);
}

/*
 * Key Service Method to preparse a payload before a key is created.
 * Check permissions and the options. Load the proposed key field
 * data into the payload for use by the instantiate method.
 */
int mktme_preparse_payload(struct key_preparsed_payload *prep)
{
	size_t datalen = prep->datalen;
	u32 *mktme_payload;
	char *options;
	int ret;

	if (datalen <= 0 || datalen > 1024 || !prep->data)
		return -EINVAL;

	options = kmemdup_nul(prep->data, datalen, GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	mktme_payload = kzalloc(sizeof(*mktme_payload), GFP_KERNEL);
	if (!mktme_payload) {
		ret = -ENOMEM;
		goto out;
	}
	ret = mktme_get_options(options, mktme_payload);
	if (ret < 0) {
		kzfree(mktme_payload);
		goto out;
	}
	prep->quotalen = sizeof(mktme_payload);
	prep->payload.data[0] = mktme_payload;
out:
	kzfree(options);
	return ret;
}

struct key_type key_type_mktme = {
	.name		= "mktme",
	.preparse	= mktme_preparse_payload,
	.free_preparse	= mktme_free_preparsed_payload,
	.instantiate	= mktme_instantiate_key,
	.describe	= user_describe,
};

static int __init init_mktme(void)
{
	int ret;

	/* Verify keys are present */
	if (mktme_nr_keyids() < 1)
		return 0;

	mktme_available_keyids = mktme_nr_keyids();

	/* Mapping of Userspace Keys to Hardware KeyIDs */
	mktme_map = kvzalloc((sizeof(*mktme_map) * (mktme_nr_keyids() + 1)),
			     GFP_KERNEL);
	if (!mktme_map)
		return -ENOMEM;

	ret = register_key_type(&key_type_mktme);
	if (!ret)
		return ret;			/* SUCCESS */

	kvfree(mktme_map);

	return -ENOMEM;
}

late_initcall(init_mktme);
