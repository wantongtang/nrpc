#include "ngx_nrpc_handler.h"
#include "controller.h"
#include "protocol.h"

namespace nrpc {

void ngx_nrpc_init_connection(ngx_connection_t *c)
{
    auto cntl = new Controller(c);
    if (!cntl->init()) {
        return cntl->finalize();
    }
    c->data = (void*)cntl;

    c->read->handler = ngx_nrpc_determine_policy;
    return ngx_nrpc_determine_policy(c->read);
}

// read one byte to determin policy
// just finalize when (timeout | eof | error)
void ngx_nrpc_determine_policy(ngx_event_t *rev)
{
    ngx_connection_t *c = (ngx_connection_t*)rev->data;
    Controller* cntl = (Controller*)c->data;

    if (rev->timedout) {
        c->timedout = 1;
        cntl->set_result(RPC_READ_TIMEOUT);
        return cntl->finalize();
    }

    char first_character;
    int n = c->recv(c, (u_char*)&first_character, 1);
    if(n == NGX_AGAIN) {
        if (!rev->timer_set) {
            ngx_add_timer(rev, cntl->read_timeout());
        }
        if (ngx_handle_read_event(rev, 0) != NGX_OK) {
            cntl->set_result(RPC_INNER_ERROR);
            cntl->set_result_text("hanlde read event error");
            return cntl->finalize();
        }
        // for next event
        return;
    }

    if((n == 0) || (n == NGX_ERROR)) {
        // read eof or error
        cntl->set_result(RPC_READ_ERROR);
        cntl->finalize();
    }

    // n == 1, determin the protocol
    c->received += 1;
    int protocol_num;
    switch(first_character) {
    case 'N':
        protocol_num = NRPC_PROTOCOL_DEFAULT_NUM/*default*/;
        break;
    case 'G':
    case 'P':
    case 'H':
        protocol_num = NRPC_PROTOCOL_HTTP_NUM/*http*/;
        break;
    default:
        protocol_num = 100/*invaild*/;
        break;
    }
    if (!cntl->set_protocol(protocol_num/*default_protocol*/)) {
        return cntl->finalize();
    }

    // get iobuf, and read the following data
    ngxplus::IOBuf* iobuf = new ngxplus::IOBuf(); // default blocksize 1024
    char* buf;
    int len = iobuf->alloc(&buf, 1, ngxplus::IOBuf::IOBUF_ALLOC_EXACT);
    if (len != 1) {
        cntl->set_result(RPC_INNER_ERROR);
        cntl->set_result_text("determin alloc error");
        return cntl->finalize();
    }
    buf[0] = first_character;
    cntl->set_iobuf(iobuf);

    rev->handler = ngx_nrpc_read_request;
    return ngx_nrpc_read_request(rev);
}

void ngx_nrpc_read_request(ngx_event_t *rev)
{
    ngx_connection_t *c = (ngx_connection_t*)rev->data;
    Controller *cntl = (Controller*)c->data;

    if (rev->timedout) {
        c->timedout = 1;
        cntl->set_result(RPC_READ_TIMEOUT);
        return cntl->finalize();
    }

    RPC_SESSION_STATE cntl_state = cntl->state();
    if (cntl_state != RPC_SESSION_READING_REQUEST) {
        cntl->set_result(RPC_INNER_ERROR);
        cntl->set_result_text("should not come into read request");
        return cntl->finalize();
    }

    ngxplus::IOBuf *iobuf = cntl->iobuf();
    ngxplus::IOBufAsZeroCopyOutputStream zero_out_stream(iobuf);

    char* buf;
    int size;
    for(;;) {
        if (!zero_out_stream.Next((void**)&buf, &size)) {
            cntl->set_result(RPC_INNER_ERROR);
            cntl->set_result_text("read request alloc error");
            cntl->finalize();
        }
        if (size == 0) {
            continue;
        }
        int len = c->recv(c, (u_char*)buf, size);
        if (len == size) {
            continue;
        }
        if (len == NGX_ERROR) {
            cntl->set_result(RPC_READ_ERROR);
            return cntl->finalize();
        }

        // read eof or again
        bool read_eof = false;
        if (len == 0) {
            read_eof = true;
        }
        Protocol* protocol = cntl->protocol();
        ParseResult presult = protocol->parse(cntl, read_eof);
        if (presult == PARSE_DONE) {
            rev->handler = ngx_nrpc_dummy_read;
            if (rev->timer_set) {
                ngx_del_timer(rev);
            }
            cntl->set_state(RPC_SESSION_PROCESSING);
            return protocol->process_request(cntl);
        }
        else if (presult == PARSE_INCOMPLETE) {
            ngx_add_timer(rev, cntl->read_timeout());
            if (ngx_handle_read_event(rev, 0) != NGX_OK) {
                cntl->set_result(RPC_INNER_ERROR);
                cntl->set_result_text("hanlde read event error");
                return cntl->finalize();
            }
            return;
        }
        else { // bad format OR broken OR bad meta
            cntl->set_result(RPC_READ_ERROR);
            if (presult == PARSE_BROKEN) {
                cntl->set_result_text("read broken");
            }
            if (presult == PARSE_BAD_FORMAT) {
                cntl->set_result_text("request bad format");
            }
            return cntl->finalize();
        }
    }
}

void ngx_nrpc_dummy_read(ngx_event_t* rev)
{
    (void) rev;
    return;
}

void ngx_nrpc_send_response(ngx_event_t* wev)
{
    ngx_connection_t* c = (ngx_connection_t*)wev->data;
    Controller* cntl = (Controller*)c->data;
    ngxplus::IOBuf* iobuf = cntl->iobuf();

    ngxplus::IOBufAsZeroCopyInputStream zero_in_stream(iobuf);
    char* buf;
    int size;
    while (zero_in_stream.Next((const void**)&buf, &size)) {
        if (size == 0) {
            continue;
        }
        int n = c->send(c, (u_char*)buf, size);
        if (n < size) {
            if (!wev->timer_set) {
                ngx_add_timer(wev, 1/*cntl->write_timeout()*/);
            }
            if (ngx_handle_write_event(wev, 0) != NGX_OK) {
                cntl->set_result(RPC_INNER_ERROR);
                cntl->set_result_text("hanlde write event error");
                return cntl->finalize();
            }
            // for the next event_loop
            return;
        }
        // n == size, read next
    }
    // send all to tcp buf
    cntl->set_state(RPC_SESSION_LOGING);
    // return cntl->get_log_handler()(cntl);
    return cntl->finalize();
}

}