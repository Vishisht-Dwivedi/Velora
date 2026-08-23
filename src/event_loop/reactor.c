#include "velora/reactor.h"

volatile sig_atomic_t running_status = RUNNING;

vr_result_t vr_reactor_create(vr_reactor_t *reactor)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }

    int e_fd = epoll_create1(EPOLL_CLOEXEC);

    if (e_fd == -1)
    {
        if (errno == EINTR)
        {
            vr_log(VR_LOG_INFO, "Shutdown called");
            return VR_INTERRUPTED;
        }

        vr_perror("Error while creating epoll file descriptor");
        return VR_ERROR;
    }

    reactor->epoll_fd = e_fd;

    vr_log(
        VR_LOG_INFO,
        "epoll fd created: %d",
        e_fd
    );

    return VR_SUCCESS;
}

vr_result_t vr_reactor_destroy(vr_reactor_t *reactor)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }

    if (close(reactor->epoll_fd) == -1)
    {
        vr_perror("Error on closing fd for epoll");
        return VR_ERROR;
    }

    reactor->epoll_fd = -1;

    vr_log(
        VR_LOG_INFO,
        "Closed epoll file descriptor"
    );

    return VR_SUCCESS;
}

vr_result_t vr_reactor_add(
    vr_reactor_t *reactor,
    vr_connection_t *conn,
    u_int32_t events
)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }

    struct epoll_event ev = {0};

    ev.events = events;
    ev.data.ptr = conn;

    int res = epoll_ctl(
        reactor->epoll_fd,
        EPOLL_CTL_ADD,
        conn->net_conn.fd,
        &ev
    );

    if (res == -1)
    {
        vr_perror("Error on adding fd to reactor events");
        return VR_ERROR;
    }

    vr_log(
        VR_LOG_INFO,
        "Added element to reactor loop successfully"
    );

    return VR_SUCCESS;
}

vr_result_t vr_reactor_wait(
    vr_reactor_t *reactor,
    int timeout
)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }

    int res = epoll_wait(
        reactor->epoll_fd,
        reactor->events,
        VR_REACTOR_MAX_EVENTS,
        timeout
    );

    if (res == -1)
    {
        if (errno == EINTR)
        {
            vr_log(VR_LOG_INFO, "Closing epoll");
            return VR_ERROR;
        }

        vr_perror("Error while waiting on epoll");
        return VR_ERROR;
    }

    reactor->ready_events = res;

    vr_log(
        VR_LOG_INFO,
        "Retrieved %d events",
        res
    );

    return VR_SUCCESS;
}

vr_result_t vr_reactor_remove(
    vr_reactor_t *reactor,
    vr_connection_t *conn
)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }

    int res = epoll_ctl(
        reactor->epoll_fd,
        EPOLL_CTL_DEL,
        conn->net_conn.fd,
        NULL
    );

    if (res == -1)
    {
        vr_perror(
            "Error on deleting fd from reactor events"
        );
        return VR_ERROR;
    }

    vr_log(
        VR_LOG_INFO,
        "Removed element from reactor loop successfully"
    );

    return VR_SUCCESS;
}

static void reactor_close_connection(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    vr_connection_t *conn,
    const char *reason
)
{
    int fd = conn->net_conn.fd;

    /*
     * Remove from epoll first while the connection object is
     * still valid.
     */
    vr_reactor_remove(
        reactor,
        conn
    );

    close(fd);

    if (vr_connection_destroy(
            manager,
            conn
        ) == VR_ERROR)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to clean up connection state for fd=%d (%s)",
            fd,
            reason
        );
    }
    else
    {
        vr_log(
            VR_LOG_INFO,
            "Closed connection fd=%d (%s)",
            fd,
            reason
        );
    }
}

/*
 * Queue a complete protocol response into conn->write_buf.
 *
 * Stage 1 write-path optimization:
 *
 *     reserve(total_size)
 *          |
 *          +--> contiguous fast path
 *          |
 *          +--> two-region wrap path
 *
 * No byte-by-byte push() operations remain here.
 *
 * Important invariant:
 *
 *     reserve() succeeds BEFORE any write-ring bytes are modified.
 *
 * Therefore a capacity/growth failure leaves the write buffer
 * untouched.
 */
static bool reactor_enqueue_response(
    vr_connection_t *conn,
    vr_packet_t *response
)
{
    if (conn == NULL || response == NULL)
    {
        vr_log(
            VR_LOG_ERROR,
            "NULL connection or response passed to enqueue"
        );

        if (response != NULL)
        {
            free(response->payload);
            free(response);
        }

        return false;
    }

    size_t packet_size =
        sizeof(vr_packet_header_t) +
        response->header.payload_len;

    /*
     * The current wire format has a uint16_t payload length, so the
     * maximum possible packet is 8 + 65535 = 65543 bytes.
     *
     * The ring-buffer reserve API accepts uint32_t.
     */
    if (packet_size > UINT32_MAX)
    {
        vr_log(
            VR_LOG_ERROR,
            "Response packet size exceeds ring-buffer API limit: %zu",
            packet_size
        );

        free(response->payload);
        free(response);

        return false;
    }

    /*
     * Reserve the COMPLETE packet before touching the write ring.
     */
    if (vr_conn_ring_buf_reserve(
            &conn->write_buf,
            (uint32_t)packet_size
        ) == VR_ERROR)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to reserve write ring space for response (%zu bytes)",
            packet_size
        );

        free(response->payload);
        free(response);

        return false;
    }

    /*
     * First contiguous writable region.
     */
    uint8_t *region1 = NULL;

    uint32_t region1_len =
        vr_conn_ring_buf_contiguous_write(
            &conn->write_buf,
            &region1
        );

    if (region1 == NULL || region1_len == 0)
    {
        /*
         * reserve() succeeded, so this should be impossible unless
         * the ring-buffer invariants were violated.
         */
        vr_log(
            VR_LOG_ERROR,
            "Write ring reserved successfully but no writable region exists"
        );

        free(response->payload);
        free(response);

        return false;
    }

    /*
     * -------------------------------------------------------------
     * FAST PATH
     * -------------------------------------------------------------
     *
     * Entire serialized packet fits in one physical ring region.
     *
     * Serialize directly into the final destination.
     */
    if (region1_len >= packet_size)
    {
        vr_packet_serialize(
            response,
            region1
        );

        /*
         * Given the successful reserve() above, this commit should
         * be guaranteed to succeed.
         */
        if (vr_conn_ring_buf_commit(
                &conn->write_buf,
                (uint32_t)packet_size
            ) == VR_ERROR)
        {
            /*
             * This indicates a broken ring-buffer invariant rather
             * than normal runtime backpressure.
             */
            vr_log(
                VR_LOG_ERROR,
                "Write ring commit failed on contiguous response"
            );

            /*
             * Do NOT attempt to continue queueing this connection's
             * data. The ring implementation must be investigated if
             * this branch is ever reached.
             */
            free(response->payload);
            free(response);

            return false;
        }

        free(response->payload);
        free(response);

        return true;
    }

    /*
     * -------------------------------------------------------------
     * WRAP PATH
     * -------------------------------------------------------------
     *
     * packet_size spans the physical end of the ring.
     *
     * reserve(packet_size) guarantees that the TOTAL free space is
     * sufficient, but the first physical region may be shorter than
     * the packet.
     *
     * We therefore:
     *
     *     serialize -> temporary contiguous buffer
     *                 |
     *                 +--> region 1
     *                 |
     *                 +--> region 2
     *
     * At most two memcpy() calls are required.
     */
    uint32_t first_part = region1_len;

    if (first_part > packet_size)
        first_part = (uint32_t)packet_size;

    uint32_t remaining =
        (uint32_t)packet_size - first_part;

    /*
     * A wrapped packet must have a non-zero second part.
     *
     * Because reserve(packet_size) already succeeded, the second
     * region is expected to be large enough after first_part is
     * committed.
     */
    if (remaining == 0)
    {
        /*
         * Defensive only; this condition should have taken the
         * contiguous fast path above.
         */
        vr_log(
            VR_LOG_ERROR,
            "Invalid wrapped response calculation"
        );

        free(response->payload);
        free(response);

        return false;
    }

    uint8_t *serialized =
        malloc(packet_size);

    if (serialized == NULL)
    {
        vr_perror(
            "Failed to allocate response staging buffer"
        );

        free(response->payload);
        free(response);

        return false;
    }

    /*
     * Serialize exactly once into a contiguous staging buffer.
     */
    vr_packet_serialize(
        response,
        serialized
    );

    /*
     * -------------------------------------------------------------
     * Pre-flight second region.
     * -------------------------------------------------------------
     *
     * We cannot inspect the second physical region until the first
     * region has been committed because contiguous_write() describes
     * the current write_pos.
     *
     * reserve(packet_size) establishes that enough TOTAL free space
     * exists. Therefore, after first_part is committed, the second
     * region must be at least `remaining`.
     *
     * In the impossible-event case where the ring invariant is
     * violated, stop rather than silently producing a truncated
     * response.
     */
    memcpy(
        region1,
        serialized,
        first_part
    );

    if (vr_conn_ring_buf_commit(
            &conn->write_buf,
            first_part
        ) == VR_ERROR)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to commit first wrapped response region"
        );

        free(serialized);
        free(response->payload);
        free(response);

        return false;
    }

    uint8_t *region2 = NULL;

    uint32_t region2_len =
        vr_conn_ring_buf_contiguous_write(
            &conn->write_buf,
            &region2
        );

    if (region2 == NULL ||
        region2_len < remaining)
    {
        /*
         * This should be unreachable if reserve()/commit() and the
         * ring-buffer invariants are correct.
         *
         * At this point first_part has already been committed, so
         * the response is necessarily inconsistent. We report the
         * invariant violation rather than pretending the packet was
         * queued successfully.
         */
        vr_log(
            VR_LOG_ERROR,
            "Write ring invariant violation: second region too small "
            "(needed=%u available=%u)",
            remaining,
            region2_len
        );

        free(serialized);
        free(response->payload);
        free(response);

        return false;
    }

    memcpy(
        region2,
        serialized + first_part,
        remaining
    );

    if (vr_conn_ring_buf_commit(
            &conn->write_buf,
            remaining
        ) == VR_ERROR)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to commit second wrapped response region"
        );

        free(serialized);
        free(response->payload);
        free(response);

        return false;
    }

    free(serialized);
    free(response->payload);
    free(response);

    return true;
}

static bool reactor_drain_writes(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    vr_connection_t *conn
)
{
    int fd = conn->net_conn.fd;

    while (!vr_conn_ring_buf_empty(
        &conn->write_buf
    ))
    {
        uint8_t *data = NULL;

        uint32_t available =
            vr_conn_ring_buf_contiguous_read(
                &conn->write_buf,
                &data
            );

        if (available == 0)
            break;

        ssize_t sent =
            vr_socket_send(
                fd,
                data,
                available,
                MSG_NOSIGNAL
            );

        if (sent > 0)
        {
            if (vr_conn_ring_buf_consume(
                    &conn->write_buf,
                    (uint32_t)sent
                ) == VR_ERROR)
            {
                vr_log(
                    VR_LOG_ERROR,
                    "Failed to consume sent bytes from write ring fd=%d",
                    fd
                );

                reactor_close_connection(
                    reactor,
                    manager,
                    conn,
                    "write ring corruption"
                );

                return true;
            }

            continue;
        }

        if (sent == -1 &&
            (errno == EAGAIN ||
             errno == EWOULDBLOCK))
        {
            break;
        }

        vr_log(
            VR_LOG_ERROR,
            "Socket write failed for fd=%d",
            fd
        );

        reactor_close_connection(
            reactor,
            manager,
            conn,
            "write error"
        );

        return true;
    }

    if (vr_conn_ring_buf_empty(
            &conn->write_buf
        ))
    {
        vr_reactor_modify(
            reactor,
            conn,
            EPOLLIN | EPOLLET
        );
    }

    return false;
}

static bool reactor_drain_reads(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    vr_connection_t *conn
)
{
    int fd = conn->net_conn.fd;

    while (true)
    {
        ssize_t len =
            vr_socket_recv_ring_buf(
                fd,
                &conn->read_buf,
                0
            );

        if (len == 0)
        {
            vr_log(
                VR_LOG_INFO,
                "Client disconnected fd=%d",
                fd
            );

            reactor_close_connection(
                reactor,
                manager,
                conn,
                "client disconnected"
            );

            return true;
        }

        if (len == -1)
        {
            if (errno == EAGAIN ||
                errno == EWOULDBLOCK)
            {
                break;
            }

            if (errno == ENOBUFS)
            {
                if (conn->read_buf.state ==
                    VR_CONN_RING_BUF_UNALLOC)
                {
                    if (vr_conn_ring_buf_init(
                            &conn->read_buf
                        ) == VR_ERROR)
                    {
                        break;
                    }

                    vr_log(
                        VR_LOG_INFO,
                        "Read ring buffer allocated cap=%u fd=%d",
                        conn->read_buf.capacity,
                        fd
                    );

                    continue;
                }

                if (vr_conn_ring_buf_grow(
                        &conn->read_buf
                    ) == VR_ERROR)
                {
                    vr_log(
                        VR_LOG_ERROR,
                        "Read buffer at max capacity, cannot grow further fd=%d",
                        fd
                    );

                    break;
                }

                vr_log(
                    VR_LOG_INFO,
                    "Grew read ring buffer cap=%u fd=%d",
                    conn->read_buf.capacity,
                    fd
                );

                continue;
            }

            vr_perror(
                "Socket read failed"
            );

            reactor_close_connection(
                reactor,
                manager,
                conn,
                "read error"
            );

            return true;
        }
    }

    return false;
}

static void reactor_process_packets(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    vr_connection_t *conn,
    bool *closed
)
{
    int fd = conn->net_conn.fd;

    while (true)
    {
        vr_packet_t pkt = {0};

        vr_result_t parsed =
            vr_parser_poll(
                &conn->parser,
                conn,
                &pkt
            );

        if (parsed == VR_EMPTY)
            break;

        if (parsed == VR_ERROR)
        {
            vr_log(
                VR_LOG_WARN,
                "Protocol violation on fd=%d",
                fd
            );

            reactor_close_connection(
                reactor,
                manager,
                conn,
                "protocol error"
            );

            *closed = true;

            return;
        }

        vr_packet_t *response =
            vr_protocol_handle_packet(
                conn,
                &pkt
            );

        /*
         * Current Stage 1 ownership model:
         *
         * parser always allocates payload storage;
         * protocol does not borrow the payload.
         *
         * True borrowed/zero-copy payload ownership is intentionally
         * deferred to a later stage.
         */
        free(pkt.payload);

        if (response == NULL)
            continue;

        if (!reactor_enqueue_response(
                conn,
                response
            ))
        {
            vr_log(
                VR_LOG_WARN,
                "Failed to enqueue response for fd=%d, "
                "deferring remaining packets",
                fd
            );

            break;
        }
    }

    if (!vr_conn_ring_buf_empty(
            &conn->write_buf
        ))
    {
        vr_reactor_modify(
            reactor,
            conn,
            EPOLLIN | EPOLLOUT | EPOLLET
        );
    }
}

static void reactor_handle_client_event(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    vr_connection_t *conn,
    uint32_t events
)
{
    bool closed = false;

    if (events & EPOLLOUT)
    {
        closed =
            reactor_drain_writes(
                reactor,
                manager,
                conn
            );
    }

    if (!closed &&
        (events & EPOLLIN))
    {
        closed =
            reactor_drain_reads(
                reactor,
                manager,
                conn
            );
    }

    if (!closed)
    {
        reactor_process_packets(
            reactor,
            manager,
            conn,
            &closed
        );
    }
}

static void reactor_handle_listener_event(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    vr_connection_t *listener_conn
)
{
    int listen_fd =
        listener_conn->net_conn.fd;

    while (true)
    {
        vr_net_conn_t new_net_conn = {0};

        vr_result_t res =
            vr_tcp_accept(
                listen_fd,
                &new_net_conn
            );

        if (res == VR_SUCCESS)
        {
            vr_connection_t *new_conn =
                vr_connection_create(
                    manager,
                    new_net_conn
                );

            if (new_conn == NULL)
            {
                close(new_net_conn.fd);
                continue;
            }

            new_conn->type =
                VR_CONN_CLIENT;

            if (vr_socket_set_non_blocking(
                    new_conn->net_conn.fd
                ) != VR_SUCCESS)
            {
                close(new_conn->net_conn.fd);

                vr_connection_destroy(
                    manager,
                    new_conn
                );

                continue;
            }

            if (vr_reactor_add(
                    reactor,
                    new_conn,
                    EPOLLIN | EPOLLET
                ) != VR_SUCCESS)
            {
                close(new_conn->net_conn.fd);

                vr_connection_destroy(
                    manager,
                    new_conn
                );

                continue;
            }

            continue;
        }

        if (res == VR_EMPTY)
            break;

        if (res == VR_INTERRUPTED)
        {
            vr_log(
                VR_LOG_INFO,
                "Stopped calling accept"
            );

            break;
        }

        vr_perror(
            "Error in socket accept"
        );

        break;
    }
}

static vr_result_t reactor_bootstrap_listener(
    vr_reactor_t *reactor,
    int port,
    vr_connection_t **out_listener_conn,
    int *out_listen_fd
)
{
    if (vr_reactor_create(
            reactor
        ) != VR_SUCCESS)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to create reactor, aborting startup"
        );

        return VR_ERROR;
    }

    int listen_fd = 0;

    if (vr_tcp_server_create(
            port,
            &listen_fd
        ) != VR_SUCCESS)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to create TCP server on port: %d",
            port
        );

        vr_reactor_destroy(
            reactor
        );

        return VR_ERROR;
    }

    vr_log(
        VR_LOG_INFO,
        "Server Started on port: %d",
        port
    );

    if (vr_socket_set_non_blocking(
            listen_fd
        ) != VR_SUCCESS)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to set listening socket to non blocking"
        );

        close(listen_fd);

        vr_reactor_destroy(
            reactor
        );

        return VR_ERROR;
    }

    vr_log(
        VR_LOG_INFO,
        "Set listening socket to non blocking"
    );

    vr_connection_t *listener_conn =
        malloc(sizeof(vr_connection_t));

    if (listener_conn == NULL)
    {
        vr_perror(
            "Malloc failed while allocating mem for listener connection in reactor loop"
        );

        close(listen_fd);

        vr_reactor_destroy(
            reactor
        );

        return VR_ERROR;
    }

    memset(
        listener_conn,
        0,
        sizeof(*listener_conn)
    );

    listener_conn->net_conn.fd =
        listen_fd;

    listener_conn->type =
        VR_CONN_LISTENER;

    if (vr_reactor_add(
            reactor,
            listener_conn,
            EPOLLIN | EPOLLET
        ) != VR_SUCCESS)
    {
        vr_log(
            VR_LOG_ERROR,
            "Failed to register listener socket with reactor"
        );

        close(listen_fd);
        free(listener_conn);

        vr_reactor_destroy(
            reactor
        );

        return VR_ERROR;
    }

    *out_listener_conn = listener_conn;
    *out_listen_fd = listen_fd;

    return VR_SUCCESS;
}

static void reactor_shutdown(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    vr_connection_t *listener_conn,
    int listen_fd
)
{
    while (manager->count > 0)
    {
        vr_connection_destroy(
            manager,
            manager->slots[0]
        );
    }

    vr_reactor_remove(
        reactor,
        listener_conn
    );

    close(listen_fd);

    free(listener_conn);

    vr_reactor_destroy(
        reactor
    );
}

vr_result_t vr_reactor_loop(
    vr_reactor_t *reactor,
    vr_connection_manager_t *manager,
    int port
)
{
    vr_connection_t *listener_conn = NULL;
    int listen_fd = -1;

    if (reactor_bootstrap_listener(
            reactor,
            port,
            &listener_conn,
            &listen_fd
        ) != VR_SUCCESS)
    {
        return VR_ERROR;
    }

    while (running_status == RUNNING)
    {
        if (vr_reactor_wait(
                reactor,
                -1
            ) != VR_SUCCESS)
        {
            break;
        }

        for (int i = 0;
             i < reactor->ready_events;
             i++)
        {
            vr_connection_t *ready_conn =
                reactor->events[i].data.ptr;

            uint32_t events =
                reactor->events[i].events;

            vr_log(
                VR_LOG_INFO,
                "Ready fd: %d",
                ready_conn->net_conn.fd
            );

            switch (ready_conn->type)
            {
                case VR_CONN_LISTENER:
                    reactor_handle_listener_event(
                        reactor,
                        manager,
                        ready_conn
                    );
                    break;

                case VR_CONN_CLIENT:
                    reactor_handle_client_event(
                        reactor,
                        manager,
                        ready_conn,
                        events
                    );
                    break;
            }
        }

        reactor->ready_events = 0;
    }

    reactor_shutdown(
        reactor,
        manager,
        listener_conn,
        listen_fd
    );

    return VR_SUCCESS;
}

vr_result_t vr_reactor_modify(
    vr_reactor_t *reactor,
    vr_connection_t *conn,
    uint32_t events
)
{
    if (reactor == NULL ||
        conn == NULL)
    {
        return VR_ERROR;
    }

    struct epoll_event ev = {0};

    ev.events = events;
    ev.data.ptr = conn;

    if (epoll_ctl(
            reactor->epoll_fd,
            EPOLL_CTL_MOD,
            conn->net_conn.fd,
            &ev
        ) == -1)
    {
        vr_perror(
            "Error modifying reactor events"
        );

        return VR_ERROR;
    }

    return VR_SUCCESS;
}