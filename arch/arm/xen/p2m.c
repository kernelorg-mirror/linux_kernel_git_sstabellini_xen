#include <linux/bootmem.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/swiotlb.h>

#include <xen/xen.h>
#include <xen/interface/memory.h>
#include <xen/swiotlb-xen.h>

#include <asm/cacheflush.h>
#include <asm/xen/page.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/interface.h>

struct xen_p2m_entry {
	unsigned long pfn;
	unsigned long mfn;
	unsigned long nr_pages;
	struct rb_node rbnode_mach;
	struct rb_node rbnode_phys;
};

spinlock_t p2m_lock;
static struct rb_root phys_to_mach = RB_ROOT;
static struct rb_root mach_to_phys = RB_ROOT;

static int xen_add_phys_to_mach_entry(struct xen_p2m_entry *new)
{
	struct rb_node **link = &phys_to_mach.rb_node;
	struct rb_node *parent = NULL;
	struct xen_p2m_entry *entry;
	int rc = 0;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct xen_p2m_entry, rbnode_phys);

		if (new->mfn == entry->mfn)
			goto err_out;
		if (new->pfn == entry->pfn)
			goto err_out;

		if (new->pfn < entry->pfn)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&new->rbnode_phys, parent, link);
	rb_insert_color(&new->rbnode_phys, &phys_to_mach);
	goto out;

err_out:
	rc = -EINVAL;
	pr_warn("%s: cannot add pfn=%pa -> mfn=%pa: pfn=%pa -> mfn=%pa already exists\n",
			__func__, &new->pfn, &new->mfn, &entry->pfn, &entry->mfn);
out:
	return rc;
}

static struct xen_p2m_entry *xen_get_p2m_entry_from_phys(unsigned long phys)
{
	struct rb_node *n = phys_to_mach.rb_node;
	struct xen_p2m_entry *entry;
	unsigned long irqflags;

	spin_lock_irqsave(&p2m_lock, irqflags);
	while (n) {
		entry = rb_entry(n, struct xen_p2m_entry, rbnode_phys);
		if (entry->pfn <= phys &&
				entry->pfn + entry->nr_pages > phys) {
			spin_unlock_irqrestore(&p2m_lock, irqflags);
			return entry;
		}
		if (phys < entry->pfn)
			n = n->rb_left;
		else
			n = n->rb_right;
	}
	spin_unlock_irqrestore(&p2m_lock, irqflags);

	return NULL;
}

static int xen_add_mach_to_phys_entry(struct xen_p2m_entry *new)
{
	struct rb_node **link = &mach_to_phys.rb_node;
	struct rb_node *parent = NULL;
	struct xen_p2m_entry *entry;
	int rc = 0;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct xen_p2m_entry, rbnode_mach);

		if (new->mfn == entry->mfn)
			goto err_out;
		if (new->pfn == entry->pfn)
			goto err_out;

		if (new->mfn < entry->mfn)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&new->rbnode_mach, parent, link);
	rb_insert_color(&new->rbnode_mach, &mach_to_phys);
	goto out;

err_out:
	rc = -EINVAL;
	pr_warn("%s: cannot add pfn=%pa -> mfn=%pa: pfn=%pa -> mfn=%pa already exists\n",
			__func__, &new->pfn, &new->mfn, &entry->pfn, &entry->mfn);
out:
	return rc;
}

static struct xen_p2m_entry *xen_get_p2m_entry_from_mach(unsigned long mfn)
{
	struct rb_node *n = mach_to_phys.rb_node;
	struct xen_p2m_entry *entry;
	unsigned long irqflags;

	spin_lock_irqsave(&p2m_lock, irqflags);
	while (n) {
		entry = rb_entry(n, struct xen_p2m_entry, rbnode_mach);
		if (entry->mfn <= mfn &&
				entry->mfn + entry->nr_pages > mfn) {
			spin_unlock_irqrestore(&p2m_lock, irqflags);
			return entry;
		}
		if (mfn < entry->mfn)
			n = n->rb_left;
		else
			n = n->rb_right;
	}
	spin_unlock_irqrestore(&p2m_lock, irqflags);

	return NULL;
}

unsigned long pfn_to_mfn(unsigned long pfn)
{
	struct xen_p2m_entry *p2m_entry;

	p2m_entry = xen_get_p2m_entry_from_phys(pfn);
	if (p2m_entry != NULL)
		return p2m_entry->mfn;

	if (xen_initial_domain())
		return pfn;
	else
		return INVALID_P2M_ENTRY;
}

unsigned long mfn_to_pfn(unsigned long mfn)
{
	struct xen_p2m_entry *p2m_entry;

	p2m_entry = xen_get_p2m_entry_from_mach(mfn);
	if (p2m_entry != NULL)
		return p2m_entry->pfn;

	if (xen_initial_domain())
		return mfn;
	else
		return INVALID_P2M_ENTRY;
}

unsigned long mfn_to_local_pfn(unsigned long mfn)
{
	struct xen_p2m_entry *p2m_entry;

	if (!xen_initial_domain())
		return INVALID_P2M_ENTRY;

	p2m_entry = xen_get_p2m_entry_from_mach(mfn);
	if (p2m_entry != NULL)
		return INVALID_P2M_ENTRY;

	return mfn;
}

bool __set_phys_to_machine_multi(unsigned long pfn,
		unsigned long mfn, unsigned long nr_pages)
{
	int rc;
	unsigned long irqflags;
	struct xen_p2m_entry *p2m_entry;

	p2m_entry = xen_get_p2m_entry_from_phys(pfn);
	if (p2m_entry != NULL) {
		BUG_ON(pfn != mfn && mfn != INVALID_P2M_ENTRY);
		spin_lock_irqsave(&p2m_lock, irqflags);
		rb_erase(&p2m_entry->rbnode_mach, &mach_to_phys);
		rb_erase(&p2m_entry->rbnode_phys, &phys_to_mach);
		spin_unlock_irqrestore(&p2m_lock, irqflags);
		kfree(p2m_entry);	
		return true;
	} else if (mfn == INVALID_P2M_ENTRY)
		return true;

	p2m_entry = kzalloc(sizeof(struct xen_p2m_entry), GFP_NOWAIT);
	if (!p2m_entry) {
		pr_warn("cannot allocate xen_p2m_entry\n");
		return false;
	}
	p2m_entry->pfn = pfn;
	p2m_entry->nr_pages = nr_pages;
	p2m_entry->mfn = mfn;

	spin_lock_irqsave(&p2m_lock, irqflags);
	if ((rc = xen_add_phys_to_mach_entry(p2m_entry) < 0) ||
		(rc = xen_add_mach_to_phys_entry(p2m_entry) < 0)) {
		spin_unlock_irqrestore(&p2m_lock, irqflags);
		return false;
	}
	spin_unlock_irqrestore(&p2m_lock, irqflags);
	return true;
}
EXPORT_SYMBOL_GPL(__set_phys_to_machine_multi);

bool __set_phys_to_machine(unsigned long pfn, unsigned long mfn)
{
	return __set_phys_to_machine_multi(pfn, mfn, 1);
}
EXPORT_SYMBOL_GPL(__set_phys_to_machine);

int p2m_init(void)
{
	spin_lock_init(&p2m_lock);
	return 0;
}
arch_initcall(p2m_init);
