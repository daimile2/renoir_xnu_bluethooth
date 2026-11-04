/* XNU MAC credential adaptation for signed networking and process-statistics entitlements.
 * socket_delegate/so_set_effective_{pid,uuid}/NECP check the supplied cred,
 * which can belong to a process other than the current worker. Retain the
 * executable's signed entitlement bits in the native credential label slot.
 * XNU copies that label on credential changes and shares it across fork;
 * exec replaces it from the new vnode, including dropping inherited grants.
 */
static int sweet_network_slot;
enum {SWEET_NETWORK_DELEGATE=1, SWEET_NETWORK_RESOLVER=2, SWEET_GLOBAL_PROC_INFO=4};

static uintptr_t
sweet_network_bits(struct vnode *vp, off_t offset)
{
	struct cs_blob *blob = ubc_cs_blob_get(vp, -1, -1, offset);
	void *ent = blob != NULL ? csblob_os_entitlements_get(blob) : NULL;
	uintptr_t bits = 0;
	if (sweet_entitlements_bool(ent, "com.apple.private.network.socket-delegate")) {
		bits |= SWEET_NETWORK_DELEGATE;
	}
	/* NECP SIGN/VALIDATE require this separate entitlement.
	 * Present on the installed mDNSResponder; absent on WebKit.Networking. */
	if (sweet_entitlements_bool(ent, "com.apple.private.validated-resolver")) {
		bits |= SWEET_NETWORK_RESOLVER;
	}
	/* proc_info.c checks this credential for cross-UID statistics. 20H392
	 * aggregated is signed with it; PowerLog runs as mobile, not root. */
	if (sweet_entitlements_bool(ent, "com.apple.private.kernel.global-proc-info")) {
		bits |= SWEET_GLOBAL_PROC_INFO;
	}
	return bits;
}

static void
sweet_network_label_init(struct label *label)
{
	mac_label_set(label, sweet_network_slot, 0);
}

static void
sweet_network_label_associate(kauth_cred_t parent, kauth_cred_t child)
{
	mac_label_set(mac_cred_label(child), sweet_network_slot,
	    mac_label_get(mac_cred_label(parent), sweet_network_slot));
}

static int
sweet_network_label_exec(kauth_cred_t old __unused, kauth_cred_t new,
    struct proc *p, struct vnode *vp, off_t offset,
    struct vnode *scriptvp __unused, struct label *vnodelabel __unused,
    struct label *scriptvnodelabel __unused, struct label *execlabel __unused,
    u_int *csflags __unused, void *attr __unused, size_t attrlen __unused,
    int *disjoint __unused)
{
	uintptr_t bits = sweet_network_bits(vp, offset);
	mac_label_set(mac_cred_label(new), sweet_network_slot, bits);
	if (bits) {
		char name[32];proc_name(proc_pid(p), name, sizeof(name));
		printf("STAR NET PRIV: exec pid=%d name=%s delegate=%u resolver=%u global_proc_info=%u\n",
		    proc_pid(p), name, !!(bits&SWEET_NETWORK_DELEGATE), !!(bits&SWEET_NETWORK_RESOLVER),
		    !!(bits&SWEET_GLOBAL_PROC_INFO));
	}
	return 0;
}

static bool
sweet_network_priv_granted(kauth_cred_t cred, int priv)
{
	uintptr_t bits = mac_label_get(mac_cred_label(cred), sweet_network_slot);
	return (priv == PRIV_NET_PRIVILEGED_SOCKET_DELEGATE && (bits&SWEET_NETWORK_DELEGATE)) ||
	       (priv == PRIV_NET_VALIDATED_RESOLVER && (bits&SWEET_NETWORK_RESOLVER)) ||
	       (priv == PRIV_GLOBAL_PROC_INFO && (bits&SWEET_GLOBAL_PROC_INFO));
}
