// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_ag.h"
#include "xfs_trace.h"

#if defined(CONFIG_XFS_LIVE_HOOKS_SRCU)
/* Initialize a notifier chain. */
void
xfs_hooks_init(
	struct xfs_hooks	*chain)
{
	srcu_init_notifier_head(&chain->head);
}

/* Make it so a function gets called whenever we hit a certain hook point. */
int
xfs_hooks_add(
	struct xfs_hooks	*chain,
	struct xfs_hook		*hook)
{
	ASSERT(hook->nb.notifier_call != NULL);
	BUILD_BUG_ON(offsetof(struct xfs_hook, nb) != 0);

	return srcu_notifier_chain_register(&chain->head, &hook->nb);
}

/* Remove a previously installed hook. */
void
xfs_hooks_del(
	struct xfs_hooks	*chain,
	struct xfs_hook		*hook)
{
	srcu_notifier_chain_unregister(&chain->head, &hook->nb);
	rcu_barrier();
}

/* Call a hook.  Returns the NOTIFY_* value returned by the last hook. */
int
xfs_hooks_call(
	struct xfs_hooks	*chain,
	unsigned long		val,
	void			*priv)
{
	return srcu_notifier_call_chain(&chain->head, val, priv);
}
#elif defined(CONFIG_XFS_LIVE_HOOKS_BLOCKING)
/* Initialize a notifier chain. */
void
xfs_hooks_init(
	struct xfs_hooks	*chain)
{
	BLOCKING_INIT_NOTIFIER_HEAD(&chain->head);
}

/* Make it so a function gets called whenever we hit a certain hook point. */
int
xfs_hooks_add(
	struct xfs_hooks	*chain,
	struct xfs_hook		*hook)
{
	ASSERT(hook->nb.notifier_call != NULL);
	BUILD_BUG_ON(offsetof(struct xfs_hook, nb) != 0);

	return blocking_notifier_chain_register(&chain->head, &hook->nb);
}

/* Remove a previously installed hook. */
void
xfs_hooks_del(
	struct xfs_hooks	*chain,
	struct xfs_hook		*hook)
{
	blocking_notifier_chain_unregister(&chain->head, &hook->nb);
}

/* Call a hook.  Returns the NOTIFY_* value returned by the last hook. */
int
xfs_hooks_call(
	struct xfs_hooks	*chain,
	unsigned long		val,
	void			*priv)
{
	return blocking_notifier_call_chain(&chain->head, val, priv);
}
#endif /* CONFIG_XFS_LIVE_HOOKS_BLOCKING */
