/*
 * Server-side mailslot management
 *
 * Copyright (C) 1998 Alexandre Julliard
 * Copyright (C) 2005 Mike McCormack
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "config.h"
#include "wine/port.h"
#include "wine/unicode.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "windef.h"
#include "winbase.h"

#include "file.h"
#include "handle.h"
#include "thread.h"
#include "request.h"

struct mailslot
{
    struct object       obj;
    struct fd          *fd;
    struct fd          *write_fd;
    unsigned int        max_msgsize;
    unsigned int        read_timeout;
    struct list         writers;
    struct list         read_q;
};

/* mailslot functions */
static void mailslot_dump( struct object*, int );
static struct fd *mailslot_get_fd( struct object * );
static void mailslot_destroy( struct object * );

static const struct object_ops mailslot_ops =
{
    sizeof(struct mailslot),   /* size */
    mailslot_dump,             /* dump */
    default_fd_add_queue,      /* add_queue */
    default_fd_remove_queue,   /* remove_queue */
    default_fd_signaled,       /* signaled */
    no_satisfied,              /* satisfied */
    mailslot_get_fd,           /* get_fd */
    mailslot_destroy           /* destroy */
};

static int mailslot_get_poll_events( struct fd * );
static void mailslot_poll_event( struct fd *, int );
static int mailslot_get_info( struct fd * );
static void mailslot_queue_async( struct fd *, void*, void*, void*, int, int );
static void mailslot_cancel_async( struct fd * );

static const struct fd_ops mailslot_fd_ops =
{
    mailslot_get_poll_events,  /* get_poll_events */
    mailslot_poll_event,       /* poll_event */
    no_flush,                  /* flush */
    mailslot_get_info,         /* get_file_info */
    mailslot_queue_async,      /* queue_async */
    mailslot_cancel_async      /* cancel_async */
};

struct mail_writer
{
    struct object         obj;
    struct mailslot      *mailslot;
    struct list           entry;
    int                   access;
    int                   sharing;
};

static void mail_writer_dump( struct object *obj, int verbose );
static struct fd *mail_writer_get_fd( struct object *obj );
static void mail_writer_destroy( struct object *obj);

static const struct object_ops mail_writer_ops =
{
    sizeof(struct mail_writer), /* size */
    mail_writer_dump,           /* dump */
    no_add_queue,               /* add_queue */
    NULL,                       /* remove_queue */
    NULL,                       /* signaled */
    NULL,                       /* satisfied */
    mail_writer_get_fd,         /* get_fd */
    mail_writer_destroy         /* destroy */
};

static int mail_writer_get_info( struct fd *fd );

static const struct fd_ops mail_writer_fd_ops =
{
    NULL,                        /* get_poll_events */
    NULL,                        /* poll_event */
    no_flush,                    /* flush */
    mail_writer_get_info,        /* get_file_info */
    no_queue_async,              /* queue_async */
    NULL                         /* cancel_async */
};

static void mailslot_destroy( struct object *obj)
{
    struct mailslot *mailslot = (struct mailslot *) obj;

    assert( mailslot->fd );
    assert( mailslot->write_fd );

    async_terminate_queue( &mailslot->read_q, STATUS_CANCELLED );

    release_object( mailslot->fd );
    release_object( mailslot->write_fd );
}

static void mailslot_dump( struct object *obj, int verbose )
{
    struct mailslot *mailslot = (struct mailslot *) obj;

    assert( obj->ops == &mailslot_ops );
    fprintf( stderr, "Mailslot max_msgsize=%d read_timeout=%d\n",
             mailslot->max_msgsize, mailslot->read_timeout );
}

static int mailslot_message_count(struct mailslot *mailslot)
{
    struct pollfd pfd;

    /* poll the socket to see if there's any messages */
    pfd.fd = get_unix_fd( mailslot->fd );
    pfd.events = POLLIN;
    pfd.revents = 0;
    return (poll( &pfd, 1, 0 ) == 1) ? 1 : 0;
}

static int mailslot_get_info( struct fd *fd )
{
    struct mailslot *mailslot = get_fd_user( fd );
    assert( mailslot->obj.ops == &mailslot_ops );
    return FD_FLAG_TIMEOUT | FD_FLAG_AVAILABLE;
}

static struct fd *mailslot_get_fd( struct object *obj )
{
    struct mailslot *mailslot = (struct mailslot *) obj;

    return (struct fd *)grab_object( mailslot->fd );
}

static int mailslot_get_poll_events( struct fd *fd )
{
    struct mailslot *mailslot = get_fd_user( fd );
    int events = 0;
    assert( mailslot->obj.ops == &mailslot_ops );

    if( !list_empty( &mailslot->read_q ))
        events |= POLLIN;

    return events;
}

static void mailslot_poll_event( struct fd *fd, int event )
{
    struct mailslot *mailslot = get_fd_user( fd );

    if( !list_empty( &mailslot->read_q ) && (POLLIN & event) )
        async_terminate_head( &mailslot->read_q, STATUS_ALERTED );

    set_fd_events( fd, mailslot_get_poll_events(fd) );
}

static void mailslot_queue_async( struct fd *fd, void *apc, void *user,
                                  void *iosb, int type, int count )
{
    struct mailslot *mailslot = get_fd_user( fd );
    int events, *ptimeout = NULL;

    assert(mailslot->obj.ops == &mailslot_ops);

    if( type != ASYNC_TYPE_READ )
    {
        set_error(STATUS_INVALID_PARAMETER);
        return;
    }

    if( list_empty( &mailslot->writers ) ||
        !mailslot_message_count( mailslot ))
    {
        set_error(STATUS_IO_TIMEOUT);
        return;
    }

    if (mailslot->read_timeout != MAILSLOT_WAIT_FOREVER)
        ptimeout = &mailslot->read_timeout;

    if (!create_async( current, ptimeout, &mailslot->read_q, apc, user, iosb ))
        return;

    /* Check if the new pending request can be served immediately */
    events = check_fd_events( fd, mailslot_get_poll_events( fd ) );
    if (events)
    {
        mailslot_poll_event( fd, events );
        return;
    }

    set_fd_events( fd, mailslot_get_poll_events( fd ));
}

static void mailslot_cancel_async( struct fd *fd )
{
    struct mailslot *mailslot = get_fd_user( fd );

    assert(mailslot->obj.ops == &mailslot_ops);
    async_terminate_queue( &mailslot->read_q, STATUS_CANCELLED );
}

static struct mailslot *create_mailslot( const WCHAR *name, size_t len, int max_msgsize,
                                         int read_timeout )
{
    struct mailslot *mailslot;
    int fds[2];
    static const WCHAR slot[] = {'m','a','i','l','s','l','o','t','\\',0};

    if( ( len <= strlenW( slot ) ) || strncmpiW( slot, name, strlenW( slot ) ) )
    {
        set_error( STATUS_OBJECT_NAME_INVALID );
        return NULL;
    }

    mailslot = create_named_object( sync_namespace, &mailslot_ops, name, len );
    if( !mailslot )
        return NULL;

    /* it already exists - there can only be one mailslot to read from */
    if( get_error() == STATUS_OBJECT_NAME_COLLISION )
    {
        release_object( mailslot );
        return NULL;
    }

    mailslot->fd = NULL;
    mailslot->write_fd = NULL;
    mailslot->max_msgsize = max_msgsize;
    mailslot->read_timeout = read_timeout;
    list_init( &mailslot->writers );
    list_init( &mailslot->read_q );

    if( !socketpair( PF_UNIX, SOCK_DGRAM, 0, fds ) )
    {
        fcntl( fds[0], F_SETFL, O_NONBLOCK );
        fcntl( fds[1], F_SETFL, O_NONBLOCK );
        mailslot->fd = create_anonymous_fd( &mailslot_fd_ops,
                                fds[1], &mailslot->obj );
        mailslot->write_fd = create_anonymous_fd( &mail_writer_fd_ops,
                                fds[0], &mailslot->obj );
        if( mailslot->fd && mailslot->write_fd ) return mailslot;
    }
    else file_set_error();

    release_object( mailslot );
    return NULL;
}

static struct mailslot *open_mailslot( const WCHAR *name, size_t len )
{
    struct object *obj;

    obj = find_object( sync_namespace, name, len );
    if (obj)
    {
        if (obj->ops == &mailslot_ops)
            return (struct mailslot *)obj;
        release_object( obj );
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
    }
    else
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );

    return NULL;
}

static void mail_writer_dump( struct object *obj, int verbose )
{
    fprintf( stderr, "Mailslot writer\n" );
}

static void mail_writer_destroy( struct object *obj)
{
    struct mail_writer *writer = (struct mail_writer *) obj;

    list_remove( &writer->entry );
    release_object( writer->mailslot );
}

static int mail_writer_get_info( struct fd *fd )
{
    return 0;
}

static struct fd *mail_writer_get_fd( struct object *obj )
{
    struct mail_writer *writer = (struct mail_writer *) obj;

    return (struct fd *)grab_object( writer->mailslot->write_fd );
}

/*
 * Readers and writers cannot be mixed.
 * If there's more than one writer, all writers must open with FILE_SHARE_WRITE
 */
static struct mail_writer *create_mail_writer( struct mailslot *mailslot, unsigned int access,
                                               unsigned int sharing )
{
    struct mail_writer *writer;

    if (!list_empty( &mailslot->writers ))
    {
        writer = LIST_ENTRY( list_head(&mailslot->writers), struct mail_writer, entry );

        if (((access & GENERIC_WRITE) || (writer->access & GENERIC_WRITE)) &&
           !((sharing & FILE_SHARE_WRITE) && (writer->sharing & FILE_SHARE_WRITE)))
        {
            set_error( STATUS_SHARING_VIOLATION );
            return 0;
        }
    }

    writer = alloc_object( &mail_writer_ops );
    if (!writer)
        return NULL;

    grab_object( mailslot );
    writer->mailslot = mailslot;
    writer->access = access;
    writer->sharing = sharing;

    list_add_head( &mailslot->writers, &writer->entry );

    return writer;
}

static struct mailslot *get_mailslot_obj( struct process *process, obj_handle_t handle,
                                          unsigned int access )
{
    struct object *obj;
    obj = get_handle_obj( process, handle, access, &mailslot_ops );
    return (struct mailslot *) obj;
}


/* create a mailslot */
DECL_HANDLER(create_mailslot)
{
    struct mailslot *mailslot;

    reply->handle = 0;
    mailslot = create_mailslot( get_req_data(), get_req_data_size(),
                                req->max_msgsize, req->read_timeout );
    if( mailslot )
    {
        reply->handle = alloc_handle( current->process, mailslot,
                                      GENERIC_READ, req->inherit );
        release_object( mailslot );
    }
}


/* open an existing mailslot */
DECL_HANDLER(open_mailslot)
{
    struct mailslot *mailslot;

    reply->handle = 0;

    if( ! ( req->sharing & FILE_SHARE_READ ) )
    {
        set_error( STATUS_SHARING_VIOLATION );
        return;
    }

    mailslot = open_mailslot( get_req_data(), get_req_data_size() );
    if( mailslot )
    {
        struct mail_writer *writer;

        writer = create_mail_writer( mailslot, req->access, req->sharing );
        if( writer )
        {
            reply->handle = alloc_handle( current->process, writer,
                                          req->access, req->inherit );
            release_object( writer );
        }
        release_object( mailslot );
    }
    else
        set_error( STATUS_NO_SUCH_FILE );
}


/* set mailslot information */
DECL_HANDLER(set_mailslot_info)
{
    struct mailslot *mailslot = get_mailslot_obj( current->process, req->handle, 0 );

    if( mailslot )
    {
        int r, fd = get_unix_fd( mailslot->fd );

        if( req->flags & MAILSLOT_SET_READ_TIMEOUT )
            mailslot->read_timeout = req->read_timeout;
        reply->max_msgsize = mailslot->max_msgsize;
        reply->read_timeout = mailslot->read_timeout;
        reply->msg_count = mailslot_message_count(mailslot);

        /* get the size of the next message */
        r = recv( fd, NULL, 0, MSG_PEEK | MSG_TRUNC );
        if( r < 0 )
            reply->next_msgsize = MAILSLOT_NO_MESSAGE;
        else
            reply->next_msgsize = r;

        release_object( mailslot );
    }
}
