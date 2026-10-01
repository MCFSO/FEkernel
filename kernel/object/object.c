/* SPDX-License-Identifier: 0BSD */
#include <fe/object.h>
#include <fe/mm/kheap.h>
#include <fe/kprintf.h>
#include <fe/panic.h>
#include <fe/string.h>
#include <fe/io.h>

static volatile u64 g_next_object_id = 1;

const char *fe_object_type_name(u32 type)
{
    switch (type) {
    case FE_OBJ_NONE:         return "空";
    case FE_OBJ_ENDPOINT:     return "端点";
    case FE_OBJ_NOTIFICATION: return "通知";
    case FE_OBJ_MEMORY:       return "内存对象";
    case FE_OBJ_TASK:         return "任务";
    case FE_OBJ_THREAD:       return "线程";
    default:                  return "未知";
    }
}

void fe_object_init(struct fe_object_header *hdr, u32 type)
{
    hdr->type = type;
    hdr->refcount = 1;
    hdr->id = g_next_object_id++;
}

void fe_object_ref(struct fe_object_header *hdr)
{
    if (!hdr) {
        return;
    }
    __atomic_add_fetch(&hdr->refcount, 1u, __ATOMIC_RELAXED);
}

/* 由各对象类型提供：释放对象自身的资源 */
void fe_object_destroy(struct fe_object_header *hdr);

void fe_object_unref(struct fe_object_header *hdr)
{
    if (!hdr) {
        return;
    }
    if (__atomic_sub_fetch(&hdr->refcount, 1u, __ATOMIC_ACQ_REL) == 0) {
        fe_object_destroy(hdr);
    }
}

/* ------------------------------------------------------------------ */
/* 句柄表                                                              */
/* ------------------------------------------------------------------ */

void fe_handle_table_init(struct fe_handle_table *t)
{
    memset(t, 0, sizeof(*t));
}

fe_handle_t fe_handle_install(struct fe_handle_table *t, struct fe_object_header *obj,
                              u32 rights)
{
    if (!t || !obj) {
        return FE_HANDLE_INVALID;
    }
    u64 irq = fe_irq_save();
    for (u32 i = 1; i < FE_HANDLE_TABLE_SIZE; i++) {   /* 0 恒为无效句柄 */
        if (t->entries[i].object == NULL) {
            fe_object_ref(obj);
            t->entries[i].object = obj;
            t->entries[i].rights = rights;
            t->used++;
            fe_irq_restore(irq);
            return i;
        }
    }
    fe_irq_restore(irq);
    return FE_HANDLE_INVALID;      /* 句柄表已满 */
}

fe_status_t fe_handle_lookup(struct fe_handle_table *t, fe_handle_t h,
                             u32 required_rights, struct fe_object_header **out)
{
    if (!t || h == FE_HANDLE_INVALID || h >= FE_HANDLE_TABLE_SIZE) {
        return FE_ERR_BADHANDLE;
    }
    struct fe_handle_entry *e = &t->entries[h];
    if (!e->object) {
        return FE_ERR_BADHANDLE;
    }
    if ((e->rights & required_rights) != required_rights) {
        return FE_ERR_ACCESS;
    }
    if (out) {
        *out = e->object;
    }
    return FE_OK;
}

/* 只读权限位：把「这个句柄现在有什么权限」当成一个事实取出来。
 * 随消息转交能力时用它收窄接收方的权限（见 fe_handle_rights 的声明说明）。 */
fe_status_t fe_handle_rights(struct fe_handle_table *t, fe_handle_t h, u32 *out_rights)
{
    if (!t || h == FE_HANDLE_INVALID || h >= FE_HANDLE_TABLE_SIZE || !out_rights) {
        return FE_ERR_BADHANDLE;
    }
    struct fe_handle_entry *e = &t->entries[h];
    if (!e->object) {
        return FE_ERR_BADHANDLE;
    }
    *out_rights = e->rights;
    return FE_OK;
}

fe_status_t fe_handle_close(struct fe_handle_table *t, fe_handle_t h)
{
    if (!t || h == FE_HANDLE_INVALID || h >= FE_HANDLE_TABLE_SIZE) {
        return FE_ERR_BADHANDLE;
    }
    u64 irq = fe_irq_save();
    struct fe_handle_entry *e = &t->entries[h];
    if (!e->object) {
        fe_irq_restore(irq);
        return FE_ERR_BADHANDLE;
    }
    struct fe_object_header *obj = e->object;
    e->object = NULL;
    e->rights = 0;
    t->used--;
    fe_irq_restore(irq);
    fe_object_unref(obj);
    return FE_OK;
}

fe_status_t fe_handle_dup(struct fe_handle_table *t, fe_handle_t h, u32 new_rights,
                          fe_handle_t *out)
{
    if (!out) {
        return FE_ERR_INVAL;
    }
    struct fe_object_header *obj = NULL;
    /* 复制本身需要 DUP 权限 */
    fe_status_t s = fe_handle_lookup(t, h, FE_RIGHT_DUP, &obj);
    if (fe_failed(s)) {
        return s;
    }
    /* 权限只能收窄：新权限必须是原权限的子集 */
    u32 old_rights = t->entries[h].rights;
    if ((new_rights & ~old_rights) != 0) {
        return FE_ERR_ACCESS;
    }
    fe_handle_t nh = fe_handle_install(t, obj, new_rights);
    if (nh == FE_HANDLE_INVALID) {
        return FE_ERR_NOMEM;
    }
    *out = nh;
    return FE_OK;
}

void fe_handle_table_clear(struct fe_handle_table *t)
{
    if (!t) {
        return;
    }
    for (u32 i = 1; i < FE_HANDLE_TABLE_SIZE; i++) {
        if (t->entries[i].object) {
            fe_handle_close(t, i);
        }
    }
}

u32 fe_handle_table_used(const struct fe_handle_table *t)
{
    return t ? t->used : 0;
}
