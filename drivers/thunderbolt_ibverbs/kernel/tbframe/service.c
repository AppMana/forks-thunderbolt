// SPDX-License-Identifier: GPL-2.0
/*
 * tbframe XDomain service glue: property directory, service driver and the
 * control-plane protocol handler. The service name is "tbframe" (protocol
 * id 1) so it coexists with the legacy "tbverbs" service during transition;
 * the generation gate, re-announce and supersede semantics come from the
 * shared thunderbolt_negotiation.h contract via core.c.
 */

#define pr_fmt(fmt) "tbframe: " fmt

#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uuid.h>

#include "tbframe_priv.h"

#define TBFRAME_PROTOCOL_KEY	"tbframe"
#define TBFRAME_PRTCID		1
#define TBFRAME_PRTCVERS	1
#define TBFRAME_PRTCREVS	1

/* Must match tbframe_wire_uuid byte-for-byte (big-endian field order). */
static const uuid_t tbframe_service_uuid =
	UUID_INIT(0x9a52c3b4, 0x1f6e, 0x4d07, 0x8b, 0x2a,
		  0x46, 0xc8, 0x1d, 0x90, 0xe5, 0xf3);

static struct tbframe *tbframe_service_tf;
static struct tb_property_dir *tbframe_service_dir;
static bool tbframe_service_driver_registered;

struct tbframe_service_binding {
	struct tbframe_link	*link;
	struct tbframe_hw	*hw;
};

/*
 * Per-link ULA EUI-64 for GID derivation (spec §8), the legacy per-rail
 * identity math: a 24-bit host hash folded from the host router UUID
 * (stable across boots, unique per host) plus a 16-bit link hash over the
 * remote UUID and route, formatted as the modified EUI-64 of the synthetic
 * locally-administered MAC 02:H1:H2:H3:L1:L2.
 */
static u32 tbframe_host_identity_hash(const u8 uuid[16])
{
	u32 h = 0;
	int i;

	if (!uuid)
		return 0x544246; /* "TBF" */
	for (i = 0; i < 16; i++)
		h = h * 31 + uuid[i];
	h &= 0xffffffu;
	return h ? h : 0x544246;
}

static u16 tbframe_link_identity_hash(const u8 remote_uuid[16], u64 route)
{
	u32 h = tbframe_host_identity_hash(remote_uuid);
	u16 folded;

	h ^= lower_32_bits(route);
	h ^= upper_32_bits(route);
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	folded = (u16)(h ^ (h >> 16));
	return folded ? folded : 1;
}

static u64 tbframe_gid_eui64(const struct tb_xdomain *xd)
{
	u32 host = tbframe_host_identity_hash(xd->local_uuid ?
					      xd->local_uuid->b : NULL);
	u16 link = tbframe_link_identity_hash(xd->remote_uuid ?
					      xd->remote_uuid->b : NULL,
					      xd->route);
	u8 mac[6] = {
		0x02, host >> 16, host >> 8, host, link >> 8, link,
	};

	/* RFC 4291 modified EUI-64: mac[0..2] ^ U/L, ff:fe, mac[3..5]. */
	return ((u64)(mac[0] ^ 0x02) << 56) | ((u64)mac[1] << 48) |
	       ((u64)mac[2] << 40) | (0xffULL << 32) | (0xfeULL << 24) |
	       ((u64)mac[3] << 16) | ((u64)mac[4] << 8) | (u64)mac[5];
}

static int tbframe_service_probe(struct tb_service *svc,
				 const struct tb_service_id *id)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct tbframe_service_binding *binding;
	struct tbframe_link *link;
	struct tbframe_hw *hw;
	int ret;

	if (!tbframe_service_tf)
		return -ENODEV;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;

	hw = tbframe_hw_create(xd);
	if (!hw) {
		ret = -ENOMEM;
		goto err_free_binding;
	}

	link = tbframe_link_create(tbframe_service_tf, &tbframe_hw_real_ops,
				   hw, xd->route, tbframe_gid_eui64(xd), true);
	if (IS_ERR(link)) {
		ret = PTR_ERR(link);
		goto err_destroy_hw;
	}

	binding->link = link;
	binding->hw = hw;
	tb_service_set_drvdata(svc, binding);
	pr_info("bound service id=%d route=0x%llx link_speed=%uGb/s width=0x%x\n",
		svc->id, xd->route, xd->link_speed, xd->link_width);
	return 0;

err_destroy_hw:
	tbframe_hw_destroy(hw);
err_free_binding:
	kfree(binding);
	return ret;
}

static void tbframe_service_remove(struct tb_service *svc)
{
	struct tbframe_service_binding *binding = tb_service_get_drvdata(svc);

	if (binding) {
		/*
		 * A forced (leaked) link teardown still references the hw
		 * context from its leaked frames; leak the hw context too
		 * rather than hand those frames a dangling pointer.
		 */
		if (tbframe_link_destroy(binding->link, TBFRAME_DOWN_UNPLUG))
			pr_err("leaking hw context after forced link teardown\n");
		else
			tbframe_hw_destroy(binding->hw);
	}
	tb_service_set_drvdata(svc, NULL);
	kfree(binding);
}

static const struct tb_service_id tbframe_service_ids[] = {
	{ TB_SERVICE(TBFRAME_PROTOCOL_KEY, TBFRAME_PRTCID) },
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, tbframe_service_ids);

static struct tb_service_driver tbframe_service_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "tbframe",
	},
	.probe = tbframe_service_probe,
	.remove = tbframe_service_remove,
	.id_table = tbframe_service_ids,
};

#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
static int tbframe_protocol_callback_xd(struct tb_xdomain *source_xd,
					const void *buf, size_t size,
					void *data)
{
	if (!data)
		return 0;
	return tbframe_handle_packet(data, source_xd, buf, size);
}
#else
static int tbframe_protocol_callback(const void *buf, size_t size, void *data)
{
	if (!data)
		return 0;
	/* Source-blind core: links are matched by route from the packet. */
	return tbframe_handle_packet(data, NULL, buf, size);
}
#endif

static struct tb_protocol_handler tbframe_protocol_handler;
static bool tbframe_protocol_handler_registered;

int tbframe_service_start(struct tbframe *tf)
{
	int ret;

	tbframe_service_tf = tf;

	memset(&tbframe_protocol_handler, 0, sizeof(tbframe_protocol_handler));
	tbframe_protocol_handler.uuid = &tbframe_service_uuid;
#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
	tbframe_protocol_handler.callback_xd = tbframe_protocol_callback_xd;
#else
	tbframe_protocol_handler.callback = tbframe_protocol_callback;
#endif
#ifdef TB_PROTOCOL_HANDLER_HAS_OWNER
	tbframe_protocol_handler.owner = THIS_MODULE;
#endif
	tbframe_protocol_handler.data = tf;
	ret = tb_register_protocol_handler(&tbframe_protocol_handler);
	if (ret)
		goto err_clear;
	tbframe_protocol_handler_registered = true;

	tbframe_service_dir = tb_property_create_dir(&tbframe_service_uuid);
	if (!tbframe_service_dir) {
		ret = -ENOMEM;
		goto err_handler;
	}
	ret = tb_property_add_immediate(tbframe_service_dir, "prtcid",
					TBFRAME_PRTCID);
	ret = ret ?: tb_property_add_immediate(tbframe_service_dir, "prtcvers",
					       TBFRAME_PRTCVERS);
	ret = ret ?: tb_property_add_immediate(tbframe_service_dir, "prtcrevs",
					       TBFRAME_PRTCREVS);
	ret = ret ?: tb_property_add_immediate(tbframe_service_dir, "prtcstns",
					       0);
	if (ret)
		goto err_dir;

	ret = tb_register_property_dir(TBFRAME_PROTOCOL_KEY,
				       tbframe_service_dir);
	if (ret)
		goto err_dir;

	ret = tb_register_service_driver(&tbframe_service_driver);
	if (ret)
		goto err_property_dir;
	tbframe_service_driver_registered = true;

	pr_info("advertised tbframe service\n");
	return 0;

err_property_dir:
	tb_unregister_property_dir(TBFRAME_PROTOCOL_KEY, tbframe_service_dir);
err_dir:
	tb_property_free_dir(tbframe_service_dir);
	tbframe_service_dir = NULL;
err_handler:
	tb_unregister_protocol_handler(&tbframe_protocol_handler);
	tbframe_protocol_handler_registered = false;
err_clear:
	tbframe_service_tf = NULL;
	return ret;
}

/*
 * Teardown order is NOT the mirror of start, deliberately.
 *
 * The protocol handler goes FIRST. tb_unregister_protocol_handler() takes
 * the core's xdomain_dispatch_lock, which the dispatch walk holds across the
 * handler callbacks, so its return is a hard guarantee that no callback of
 * ours is running and none can start again (drivers/thunderbolt/xdomain.c,
 * commit 054b92c). Everything after this point -- unbinding every service,
 * destroying every link, tearing down rings, HopIDs and paths -- therefore
 * runs with inbound XDomain requests structurally excluded, instead of
 * merely racing them under a flag. That is the property incident 1 needs:
 * a peer that keeps HELLOing through our unload cannot touch a link that is
 * being taken apart.
 *
 * The cost is one retry interval of unanswered HELLOs for a peer that is
 * mid-handshake, which the peer's own retry budget already absorbs.
 *
 * Start order keeps the handler first for the opposite reason: the widest
 * possible window in which an inbound HELLO can be answered. In both cases
 * the handler brackets everything else.
 */
void tbframe_service_stop(struct tbframe *tf)
{
	if (tbframe_protocol_handler_registered) {
		tb_unregister_protocol_handler(&tbframe_protocol_handler);
		tbframe_protocol_handler_registered = false;
	}
	if (tbframe_service_driver_registered) {
		/* Unbinds every service, destroying its link. */
		tb_unregister_service_driver(&tbframe_service_driver);
		tbframe_service_driver_registered = false;
	}
	if (tbframe_service_dir) {
		tb_unregister_property_dir(TBFRAME_PROTOCOL_KEY,
					   tbframe_service_dir);
		tb_property_free_dir(tbframe_service_dir);
		tbframe_service_dir = NULL;
	}
	tbframe_service_tf = NULL;
}
