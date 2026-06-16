// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "tbv.h"

struct tbv_name_map {
	const char *name;
	int value;
};

static int tbv_parse_enum(const char *input, const struct tbv_name_map *map,
			  const char *param)
{
	int i;

	if (!input || !*input)
		return -EINVAL;

	for (i = 0; map[i].name; i++) {
		if (!strcmp(input, map[i].name))
			return map[i].value;
	}

	pr_err("invalid %s='%s'\n", param, input);
	return -EINVAL;
}

static int tbv_parse_lanes(struct tbv_config *cfg, const char *input)
{
	unsigned int min;
	unsigned int max;
	int ret;

	if (!input || !*input || !strcmp(input, "auto")) {
		cfg->lanes_auto = true;
		cfg->lanes_min = 0;
		cfg->lanes_max = 0;
		return 0;
	}

	ret = sscanf(input, "%u-%u", &min, &max);
	if (ret == 1) {
		max = min;
	} else if (ret != 2) {
		pr_err("invalid lanes='%s'\n", input);
		return -EINVAL;
	}

	if (!min || max < min) {
		pr_err("invalid lanes range '%s'\n", input);
		return -EINVAL;
	}

	cfg->lanes_auto = false;
	cfg->lanes_min = min;
	cfg->lanes_max = max;
	return 0;
}

int tbv_config_parse(struct tbv_config *cfg, const char *compat,
		     const char *profile, const char *tbnet,
		     const char *tbnet_identity, const char *lanes)
{
	static const struct tbv_name_map compat_map[] = {
		{ "auto", TBV_COMPAT_AUTO },
		{ "force", TBV_COMPAT_FORCE },
		{ "off", TBV_COMPAT_OFF },
		{ }
	};
	static const struct tbv_name_map profile_map[] = {
		{ "auto", TBV_PROFILE_AUTO },
		{ "mac_compat", TBV_PROFILE_MAC_COMPAT },
		{ "linux_perf", TBV_PROFILE_LINUX_PERF },
		{ "mixed", TBV_PROFILE_MIXED },
		{ }
	};
	static const struct tbv_name_map tbnet_map[] = {
		{ "auto", TBV_TBNET_AUTO },
		{ "allow", TBV_TBNET_ALLOW },
		{ "prefer_rdma", TBV_TBNET_PREFER_RDMA },
		{ "block", TBV_TBNET_BLOCK },
		{ }
	};
	static const struct tbv_name_map identity_map[] = {
		{ "auto", TBV_TBNET_ID_AUTO },
		{ "stock", TBV_TBNET_ID_STOCK },
		{ "stock_proxy", TBV_TBNET_ID_STOCK_PROXY },
		{ "minimal_packet", TBV_TBNET_ID_MINIMAL_PACKET },
		{ "off", TBV_TBNET_ID_OFF },
		{ }
	};
	int value;
	int ret;

	memset(cfg, 0, sizeof(*cfg));

	value = tbv_parse_enum(compat, compat_map, "compat");
	if (value < 0)
		return value;
	cfg->compat = value;

	value = tbv_parse_enum(profile, profile_map, "profile");
	if (value < 0)
		return value;
	cfg->profile = value;

	value = tbv_parse_enum(tbnet, tbnet_map, "tbnet");
	if (value < 0)
		return value;
	cfg->tbnet = value;

	value = tbv_parse_enum(tbnet_identity, identity_map,
			       "tbnet_identity");
	if (value < 0)
		return value;
	cfg->tbnet_identity = value;

	ret = tbv_parse_lanes(cfg, lanes);
	if (ret)
		return ret;

	return 0;
}

static enum tbv_profile tbv_resolve_profile(const struct tbv_config *cfg)
{
	if (cfg->profile != TBV_PROFILE_AUTO)
		return cfg->profile;

	switch (cfg->compat) {
	case TBV_COMPAT_FORCE:
		return TBV_PROFILE_MAC_COMPAT;
	case TBV_COMPAT_OFF:
		return TBV_PROFILE_LINUX_PERF;
	case TBV_COMPAT_AUTO:
	default:
		return TBV_PROFILE_MIXED;
	}
}

static enum tbv_tbnet_identity_mode
tbv_resolve_tbnet_identity(const struct tbv_config *cfg,
			   enum tbv_profile profile)
{
	if (cfg->tbnet_identity != TBV_TBNET_ID_AUTO)
		return cfg->tbnet_identity;

	if (profile == TBV_PROFILE_MAC_COMPAT || profile == TBV_PROFILE_MIXED)
		return TBV_TBNET_ID_MINIMAL_PACKET;

	return TBV_TBNET_ID_OFF;
}

int tbv_config_resolve(struct tbv_resolved_config *resolved,
		       const struct tbv_config *cfg)
{
	enum tbv_profile profile;

	memset(resolved, 0, sizeof(*resolved));
	resolved->requested = *cfg;

	profile = tbv_resolve_profile(cfg);

	if (cfg->compat == TBV_COMPAT_FORCE &&
	    profile == TBV_PROFILE_LINUX_PERF) {
		pr_err("compat=force conflicts with profile=linux_perf\n");
		return -EINVAL;
	}

	if (cfg->compat == TBV_COMPAT_OFF &&
	    profile == TBV_PROFILE_MAC_COMPAT) {
		pr_err("compat=off conflicts with profile=mac_compat\n");
		return -EINVAL;
	}

	resolved->profile = profile;
	resolved->tbnet_identity = tbv_resolve_tbnet_identity(cfg, profile);
	resolved->native_enabled = profile == TBV_PROFILE_LINUX_PERF ||
				   profile == TBV_PROFILE_MIXED;
	resolved->apple_enabled = profile == TBV_PROFILE_MAC_COMPAT ||
				  profile == TBV_PROFILE_MIXED;
	resolved->rc_supported = resolved->native_enabled;
	resolved->uc_supported = resolved->native_enabled ||
				 resolved->apple_enabled;

	return 0;
}

const char *tbv_compat_name(enum tbv_compat_mode mode)
{
	switch (mode) {
	case TBV_COMPAT_AUTO:
		return "auto";
	case TBV_COMPAT_FORCE:
		return "force";
	case TBV_COMPAT_OFF:
		return "off";
	default:
		return "unknown";
	}
}

const char *tbv_profile_name(enum tbv_profile profile)
{
	switch (profile) {
	case TBV_PROFILE_AUTO:
		return "auto";
	case TBV_PROFILE_MAC_COMPAT:
		return "mac_compat";
	case TBV_PROFILE_LINUX_PERF:
		return "linux_perf";
	case TBV_PROFILE_MIXED:
		return "mixed";
	default:
		return "unknown";
	}
}

const char *tbv_tbnet_policy_name(enum tbv_tbnet_policy policy)
{
	switch (policy) {
	case TBV_TBNET_AUTO:
		return "auto";
	case TBV_TBNET_ALLOW:
		return "allow";
	case TBV_TBNET_PREFER_RDMA:
		return "prefer_rdma";
	case TBV_TBNET_BLOCK:
		return "block";
	default:
		return "unknown";
	}
}

const char *tbv_tbnet_identity_name(enum tbv_tbnet_identity_mode mode)
{
	switch (mode) {
	case TBV_TBNET_ID_AUTO:
		return "auto";
	case TBV_TBNET_ID_STOCK:
		return "stock";
	case TBV_TBNET_ID_STOCK_PROXY:
		return "stock_proxy";
	case TBV_TBNET_ID_MINIMAL_PACKET:
		return "minimal_packet";
	case TBV_TBNET_ID_OFF:
		return "off";
	default:
		return "unknown";
	}
}

const char *tbv_backend_name(enum tbv_backend_type type)
{
	switch (type) {
	case TBV_BACKEND_NATIVE:
		return "native";
	case TBV_BACKEND_APPLE:
		return "apple";
	default:
		return "unknown";
	}
}
