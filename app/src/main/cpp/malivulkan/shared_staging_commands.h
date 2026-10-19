/* Synchronous CPU copy source; never imported into Vulkan or referenced by GPU. */
static uint32_t native_shared_command(struct vk_session *s, uint32_t op, const uint8_t *w,
        uint32_t bytes, int *reply_fd, VkResult *result) {
    uint32_t expected = op==MB_SHARED_REGISTER ? 16 : op==MB_SHARED_WRITE ? 32 : 8;
    if (bytes!=expected) return MB_PROTOCOL_ERROR;
    struct native_device *d=NULL;
    for (unsigned i=0;i<MB_MAX_LOGICAL_DEVICES;++i)
        if(s->logical[i].handle && s->logical[i].id==mb_get_u32(w)) { d=&s->logical[i];break; }
    if (!d || !d->interop.enabled || !d->normal.enabled) { *result=VK_ERROR_FEATURE_NOT_PRESENT; return MB_UNSUPPORTED; }
    if (d->submit.lost) { *result=VK_ERROR_DEVICE_LOST; return MB_VULKAN_ERROR; }
    struct native_memory *m=interop_find_memory(d,mb_get_u32(w+4));
    if (!m || m->ahb || !m->map || interop_memory_pending(d,m) ||
        (d->interop.properties.memoryTypes[m->type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return MB_PROTOCOL_ERROR;
    if (op==MB_SHARED_DROP) { mb_shared_release(&m->shared); return MB_OK; }
    if (op==MB_SHARED_REGISTER) {
        uint64_t size=mb_get_u64(w+8); unsigned regions=0;
        if(m->shared.map || size!=m->size || m->map_offset || m->map_size!=size || !mb_shared_file_bytes(size)) return MB_PROTOCOL_ERROR;
        for(unsigned i=0;i<MB_MAX_LOGICAL_DEVICES;++i)
            for(unsigned j=0;j<MB_SUBMIT_MAX_OBJECTS;++j) regions+=s->logical[i].interop.memories[j].shared.map!=NULL;
        if(regions>=MB_SHARED_MAX_REGIONS) { *result=VK_ERROR_FEATURE_NOT_PRESENT; return MB_UNSUPPORTED; }
        *reply_fd=mb_shared_create(&m->shared,size);
        if(*reply_fd<0) { *result=VK_ERROR_FEATURE_NOT_PRESENT; return MB_UNSUPPORTED; }
        LOG("CPU shared staging registered memory=%u bytes=%llu (read-only broker mapping)",m->id,(unsigned long long)size);
        return MB_OK;
    }
    uint64_t offset=mb_get_u64(w+8), size=mb_get_u64(w+16), sequence=mb_get_u64(w+24);
    if (!m->shared.map || m->shared.bytes!=m->map_size || m->map_offset ||
        !mb_interop_mapped_range(m->size,m->map_offset,m->map_size,offset,size) ||
        m->shared.sequence==UINT64_MAX || sequence!=m->shared.sequence+1 ||
        __atomic_load_n(mb_shared_sequence(m->shared.map,m->shared.bytes),__ATOMIC_ACQUIRE)!=sequence) return MB_PROTOCOL_ERROR;
    struct dd_ft_id trace={0}; trace.scope=d->normal.trace_scope;
    const int record=broker_trace_detailed;
    if(record) dd_ft_event(DD_FT_BROKER_SHARED_COPY_BEGIN,trace,m->id,offset,0,1);
    memcpy((uint8_t *)m->map+offset,(uint8_t *)m->shared.map+offset,(size_t)size);
    if(record) dd_ft_event(DD_FT_BROKER_SHARED_COPY_END,trace,m->id,offset,0,1);
    if(__atomic_load_n(mb_shared_sequence(m->shared.map,m->shared.bytes),__ATOMIC_ACQUIRE)!=sequence) return MB_PROTOCOL_ERROR;
    m->shared.sequence=sequence;
    return MB_OK; /* The original reply follows this completed CPU copy. */
}
