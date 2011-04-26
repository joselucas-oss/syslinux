#include <syslinux/sysappend.h>
#include <ctype.h>
#include <lwip/api.h>
#include "pxe.h"
#include "../../../version.h"
#include "url.h"

#define HTTP_PORT	80

static bool is_tspecial(int ch)
{
    bool tspecial = false;
    switch(ch) {
    case '(':  case ')':  case '<':  case '>':  case '@':
    case ',':  case ';':  case ':':  case '\\': case '"':
    case '/':  case '[':  case ']':  case '?':  case '=':
    case '{':  case '}':  case ' ':  case '\t':
	tspecial = true;
	break;
    }
    return tspecial;
}

static bool is_ctl(int ch)
{
    return ch < 0x20;
}

static bool is_token(int ch)
{
    /* Can by antying except a ctl character or a tspecial */
    return !is_ctl(ch) && !is_tspecial(ch);
}

static bool append_ch(char *str, size_t size, size_t *pos, int ch)
{
    bool success = true;
    if ((*pos + 1) >= size) {
	*pos = 0;
	success = false;
    } else {
	str[*pos] = ch;
	str[*pos + 1] = '\0';
	*pos += 1;
    }
    return success;
}

static size_t cookie_len, header_len;
static char *cookie_buf, *header_buf;

extern uint32_t SendCookies;

static size_t http_do_bake_cookies(char *q)
{
    static const char uchexchar[16] = "0123456789ABCDEF";
    int i;
    size_t len;
    size_t n = 0;
    const char *p;
    char c;
    size_t qlen = q ? -1UL : 0;
    bool first = true;
    uint32_t mask = SendCookies;

    for (i = 0; i < SYSAPPEND_MAX; i++) {
	if ((mask & 1) && (p = sysappend_strings[i])) {
	    len = snprintf(q, qlen, "%s_Syslinux_", first ? "Cookie: " : "");
	    if (q)
		q += len;
	    n += len;
	    first = false;
	    /* Copy string up to and including '=' */
	    do {
		c = *p++;
		if (q)
		    *q++ = c;
		n++;
	    } while (c != '=');
	    while ((c = *p++)) {
		if (c == ' ') {
		    if (q)
			*q++ = '+';
		    n++;
		} else if (is_token(c)) {
		    if (q)
			*q++ = c;
		    n++;
		} else {
		    if (q) {
			*q++ = '%';
			*q++ = uchexchar[c >> 4];
			*q++ = uchexchar[c & 15];
		    }
		    n += 3;
		}
	    }
	    if (q)
		*q++ = ';';
	    n++;
	}
	mask >>= 1;
    }
    if (!first) {
	if (q) {
	    *q++ = '\r';
	    *q++ = '\n';
	}
	n += 2;
    }
    if (q)
	*q = '\0';
    
    return n;
}

void http_bake_cookies(void)
{
    if (cookie_buf)
	free(cookie_buf);

    cookie_len = http_do_bake_cookies(NULL);
    cookie_buf = malloc(cookie_len+1);
    if (!cookie_buf) {
	cookie_len = 0;
	return;
    }

    if (header_buf)
	free(header_buf);

    header_len = cookie_len + 6*FILENAME_MAX + 256;
    header_buf = malloc(header_len);
    if (!header_buf) {
	header_len = 0;
	return;			/* Uh-oh... */
    }

    http_do_bake_cookies(cookie_buf);
}

void http_open(struct url_info *url, struct inode *inode, const char **redir)
{
    struct pxe_pvt_inode *socket = PVT(inode);
    int header_bytes;
    const char *next;
    char field_name[20];
    char field_value[1024];
    size_t field_name_len, field_value_len;
    err_t err;
    enum state {
	st_httpver,
	st_stcode,
	st_skipline,
	st_fieldfirst,
	st_fieldname,
	st_fieldvalue,
	st_skip_fieldname,
	st_skip_fieldvalue,
	st_eoh,
    } state;
    struct ip_addr addr;
    static char location[FILENAME_MAX];
    uint32_t content_length; /* same as inode->size */
    size_t response_size;
    int status;
    int pos;

    if (!header_buf)
	return;			/* http is broken... */

    socket->fill_buffer = tcp_fill_buffer;
    socket->close = tcp_close_file;

    /* Reset all of the variables */
    inode->size = content_length = -1;

    /* Start the http connection */
    socket->conn = netconn_new(NETCONN_TCP);
    if (!socket->conn) {
	printf("netconn_new failed\n");
        return;
    }

    addr.addr = url->ip;
    if (!url->port)
	url->port = HTTP_PORT;

    err = netconn_connect(socket->conn, &addr, url->port);
    if (err) {
	printf("netconn_connect error %d\n", err);
	goto fail;
    }

    strcpy(header_buf, "GET /");
    header_bytes = 5;
    header_bytes += url_escape_unsafe(header_buf+5, url->path,
				      sizeof header_buf - 5);
    if (header_bytes > header_len)
	goto fail;		/* Buffer overflow */
    header_bytes += snprintf(header_buf + header_bytes,
			     header_len - header_bytes,
			     " HTTP/1.0\r\n"
			     "Host: %s\r\n"
			     "User-Agent: PXELINUX/" VERSION_STR "\r\n"
			     "Connection: close\r\n"
			     "%s"
			     "\r\n",
			     url->host, cookie_buf ? cookie_buf : "");
    if (header_bytes > header_len)
	goto fail;		/* Buffer overflow */

    err = netconn_write(socket->conn, header_buf,
			header_bytes, NETCONN_NOCOPY);
    if (err) {
	printf("netconn_write error %d\n", err);
	goto fail;
    }

    /* Parse the HTTP header */
    state = st_httpver;
    pos = 0;
    status = 0;
    response_size = 0;
    field_value_len = 0;
    field_name_len = 0;

    while (state != st_eoh) {
	int ch = pxe_getc(inode);
	/* Eof before I finish paring the header */
	if (ch == -1)
	    goto fail;
#if 0
        printf("%c", ch);
#endif
	response_size++;
	if (ch == '\r' || ch == '\0')
	    continue;
	switch (state) {
	case st_httpver:
	    if (ch == ' ') {
		state = st_stcode;
		pos = 0;
	    }
	    break;

	case st_stcode:
	    if (ch < '0' || ch > '9')
	       goto fail;
	    status = (status*10) + (ch - '0');
	    if (++pos == 3)
		state = st_skipline;
	    break;

	case st_skipline:
	    if (ch == '\n')
		state = st_fieldfirst;
	    break;

	case st_fieldfirst:
	    if (ch == '\n')
		state = st_eoh;
	    else if (isspace(ch)) {
		/* A continuation line */
		state = st_fieldvalue;
		goto fieldvalue;
	    }
	    else if (is_token(ch)) {
		/* Process the previous field before starting on the next one */
		if (strcasecmp(field_name, "Content-Length") == 0) {
		    next = field_value;
		    /* Skip leading whitespace */
		    while (isspace(*next))
			next++;
		    content_length = 0;
		    for (;(*next >= '0' && *next <= '9'); next++) {
			if ((content_length * 10) < content_length)
			    break;
			content_length = (content_length * 10) + (*next - '0');
		    }
		    /* In the case of overflow or other error ignore
		     * Content-Length.
		     */
		    if (*next)
			content_length = -1;
		}
		else if (strcasecmp(field_name, "Location") == 0) {
		    next = field_value;
		    /* Skip leading whitespace */
		    while (isspace(*next))
			next++;
		    strlcpy(location, next, sizeof location);
		}
		/* Start the field name and field value afress */
		field_name_len = 1;
		field_name[0] = ch;
		field_name[1] = '\0';
		field_value_len = 0;
		field_value[0] = '\0';
		state = st_fieldname;
	    }
	    else /* Bogus try to recover */
		state = st_skipline;
	    break;

	case st_fieldname:
	    if (ch == ':' ) {
		state = st_fieldvalue;
	    }
	    else if (is_token(ch)) {
		if (!append_ch(field_name, sizeof field_name, &field_name_len, ch))
		    state = st_skip_fieldname;
	    }
	    /* Bogus cases try to recover */
	    else if (ch == '\n')
		state = st_fieldfirst;
	    else
		state = st_skipline;
	    break;

	 case st_fieldvalue:
	    if (ch == '\n')
		state = st_fieldfirst;
	    else {
	    fieldvalue:
		if (!append_ch(field_value, sizeof field_value, &field_value_len, ch))
		    state = st_skip_fieldvalue;
	    }
	    break;

	/* For valid fields whose names are longer than I choose to support. */
	case st_skip_fieldname:
	    if (ch == ':')
		state = st_skip_fieldvalue;
	    else if (is_token(ch))
		state = st_skip_fieldname;
	    /* Bogus cases try to recover */
	    else if (ch == '\n')
		state = st_fieldfirst;
	    else
		state = st_skipline;
	    break;

	/* For valid fields whose bodies are longer than I choose to support. */
	case st_skip_fieldvalue:
	    if (ch == '\n')
		state = st_fieldfirst;
	    break;

	case st_eoh:
	   break; /* Should never happen */
	}
    }

    if (state != st_eoh)
	status = 0;

    switch (status) {
    case 200:
	/*
	 * All OK, need to mark header data consumed and set up a file
	 * structure...
	 */
	/* Treat the remainder of the bytes as data */
	socket->tftp_filepos -= response_size;
	break;
    case 301:
    case 302:
    case 303:
    case 307:
	/* A redirect */
	if (!location[0])
	    goto fail;
	*redir = location;
	goto fail;
    default:
	goto fail;
	break;
    }
    return;
fail:
    inode->size = 0;
    tcp_close_file(inode);
    return;
}
