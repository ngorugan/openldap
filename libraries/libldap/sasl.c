/* $OpenLDAP$ */
/*
 * Copyright 1998-2000 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

/*
 *	BindRequest ::= SEQUENCE {
 *		version		INTEGER,
 *		name		DistinguishedName,	 -- who
 *		authentication	CHOICE {
 *			simple		[0] OCTET STRING -- passwd
#ifdef LDAP_API_FEATURE_X_OPENLDAP_V2_KBIND
 *			krbv42ldap	[1] OCTET STRING
 *			krbv42dsa	[2] OCTET STRING
#endif
 *			sasl		[3] SaslCredentials	-- LDAPv3
 *		}
 *	}
 *
 *	BindResponse ::= SEQUENCE {
 *		COMPONENTS OF LDAPResult,
 *		serverSaslCreds		OCTET STRING OPTIONAL -- LDAPv3
 *	}
 *
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>
#include <ac/errno.h>

#include "ldap-int.h"


/*
 * ldap_sasl_bind - bind to the ldap server (and X.500).
 * The dn (usually NULL), mechanism, and credentials are provided.
 * The message id of the request initiated is provided upon successful
 * (LDAP_SUCCESS) return.
 *
 * Example:
 *	ldap_sasl_bind( ld, NULL, "mechanism",
 *		cred, NULL, NULL, &msgid )
 */

int
ldap_sasl_bind(
	LDAP			*ld,
	LDAP_CONST char	*dn,
	LDAP_CONST char	*mechanism,
	struct berval	*cred,
	LDAPControl		**sctrls,
	LDAPControl		**cctrls,
	int				*msgidp )
{
	BerElement	*ber;
	int rc;

	Debug( LDAP_DEBUG_TRACE, "ldap_sasl_bind\n", 0, 0, 0 );

	assert( ld != NULL );
	assert( LDAP_VALID( ld ) );
	assert( msgidp != NULL );

	if( msgidp == NULL ) {
		ld->ld_errno = LDAP_PARAM_ERROR;
		return ld->ld_errno;
	}

	if( mechanism == LDAP_SASL_SIMPLE ) {
		if( dn == NULL && cred != NULL ) {
			/* use default binddn */
			dn = ld->ld_defbinddn;
		}

	} else if( ld->ld_version < LDAP_VERSION3 ) {
		ld->ld_errno = LDAP_NOT_SUPPORTED;
		return ld->ld_errno;
	}

	if ( dn == NULL ) {
		dn = "";
	}

	/* create a message to send */
	if ( (ber = ldap_alloc_ber_with_options( ld )) == NULL ) {
		ld->ld_errno = LDAP_NO_MEMORY;
		return ld->ld_errno;
	}

	assert( BER_VALID( ber ) );

	if( mechanism == LDAP_SASL_SIMPLE ) {
		/* simple bind */
		rc = ber_printf( ber, "{it{istO}" /*}*/,
			++ld->ld_msgid, LDAP_REQ_BIND,
			ld->ld_version, dn, LDAP_AUTH_SIMPLE,
			cred );
		
	} else if ( cred == NULL ) {
		/* SASL bind w/o creditials */
		rc = ber_printf( ber, "{it{ist{s}}" /*}*/,
			++ld->ld_msgid, LDAP_REQ_BIND,
			ld->ld_version, dn, LDAP_AUTH_SASL,
			mechanism );

	} else {
		/* SASL bind w/ creditials */
		rc = ber_printf( ber, "{it{ist{sO}}" /*}*/,
			++ld->ld_msgid, LDAP_REQ_BIND,
			ld->ld_version, dn, LDAP_AUTH_SASL,
			mechanism, cred );
	}

	if( rc == -1 ) {
		ld->ld_errno = LDAP_ENCODING_ERROR;
		ber_free( ber, 1 );
		return( -1 );
	}

	/* Put Server Controls */
	if( ldap_int_put_controls( ld, sctrls, ber ) != LDAP_SUCCESS ) {
		ber_free( ber, 1 );
		return ld->ld_errno;
	}

	if ( ber_printf( ber, /*{*/ "}" ) == -1 ) {
		ld->ld_errno = LDAP_ENCODING_ERROR;
		ber_free( ber, 1 );
		return ld->ld_errno;
	}

#ifndef LDAP_NOCACHE
	if ( ld->ld_cache != NULL ) {
		ldap_flush_cache( ld );
	}
#endif /* !LDAP_NOCACHE */

	/* send the message */
	*msgidp = ldap_send_initial_request( ld, LDAP_REQ_BIND, dn, ber );

	if(*msgidp < 0)
		return ld->ld_errno;

	return LDAP_SUCCESS;
}


int
ldap_sasl_bind_s(
	LDAP			*ld,
	LDAP_CONST char	*dn,
	LDAP_CONST char	*mechanism,
	struct berval	*cred,
	LDAPControl		**sctrls,
	LDAPControl		**cctrls,
	struct berval	**servercredp )
{
	int	rc, msgid;
	LDAPMessage	*result;
	struct berval	*scredp = NULL;

	Debug( LDAP_DEBUG_TRACE, "ldap_sasl_bind_s\n", 0, 0, 0 );

	/* do a quick !LDAPv3 check... ldap_sasl_bind will do the rest. */
	if( servercredp != NULL ) {
		if (ld->ld_version < LDAP_VERSION3) {
			ld->ld_errno = LDAP_NOT_SUPPORTED;
			return ld->ld_errno;
		}
		*servercredp = NULL;
	}

	rc = ldap_sasl_bind( ld, dn, mechanism, cred, sctrls, cctrls, &msgid );

	if ( rc != LDAP_SUCCESS ) {
		return( rc );
	}

	if ( ldap_result( ld, msgid, 1, NULL, &result ) == -1 ) {
		return( ld->ld_errno );	/* ldap_result sets ld_errno */
	}

	/* parse the results */
	scredp = NULL;
	if( servercredp != NULL ) {
		rc = ldap_parse_sasl_bind_result( ld, result, &scredp, 0 );
	}

	if ( rc != LDAP_SUCCESS && rc != LDAP_SASL_BIND_IN_PROGRESS ) {
		ldap_msgfree( result );
		return( rc );
	}

	rc = ldap_result2error( ld, result, 1 );

	if ( rc == LDAP_SUCCESS || rc == LDAP_SASL_BIND_IN_PROGRESS ) {
		if( servercredp != NULL ) {
			*servercredp = scredp;
			scredp = NULL;
		}
	}

	if ( scredp != NULL ) {
		ber_bvfree(scredp);
	}

	return rc;
}


/*
* Parse BindResponse:
*
*   BindResponse ::= [APPLICATION 1] SEQUENCE {
*     COMPONENTS OF LDAPResult,
*     serverSaslCreds  [7] OCTET STRING OPTIONAL }
*
*   LDAPResult ::= SEQUENCE {
*     resultCode      ENUMERATED,
*     matchedDN       LDAPDN,
*     errorMessage    LDAPString,
*     referral        [3] Referral OPTIONAL }
*/

int
ldap_parse_sasl_bind_result(
	LDAP			*ld,
	LDAPMessage		*res,
	struct berval	**servercredp,
	int				freeit )
{
	ber_int_t errcode;
	struct berval* scred;

	ber_tag_t tag;
	BerElement	*ber;

	Debug( LDAP_DEBUG_TRACE, "ldap_parse_sasl_bind_result\n", 0, 0, 0 );

	assert( ld != NULL );
	assert( LDAP_VALID( ld ) );
	assert( res != NULL );

	if ( ld == NULL || res == NULL ) {
		return LDAP_PARAM_ERROR;
	}

	if( servercredp != NULL ) {
		if( ld->ld_version < LDAP_VERSION2 ) {
			return LDAP_NOT_SUPPORTED;
		}
		*servercredp = NULL;
	}

	if( res->lm_msgtype != LDAP_RES_BIND ) {
		ld->ld_errno = LDAP_PARAM_ERROR;
		return ld->ld_errno;
	}

	scred = NULL;

	if ( ld->ld_error ) {
		LDAP_FREE( ld->ld_error );
		ld->ld_error = NULL;
	}
	if ( ld->ld_matched ) {
		LDAP_FREE( ld->ld_matched );
		ld->ld_matched = NULL;
	}

	/* parse results */

	ber = ber_dup( res->lm_ber );

	if( ber == NULL ) {
		ld->ld_errno = LDAP_NO_MEMORY;
		return ld->ld_errno;
	}

	if ( ld->ld_version < LDAP_VERSION2 ) {
		tag = ber_scanf( ber, "{ia}",
			&errcode, &ld->ld_error );

		if( tag == LBER_ERROR ) {
			ber_free( ber, 0 );
			ld->ld_errno = LDAP_DECODING_ERROR;
			return ld->ld_errno;
		}

	} else {
		ber_len_t len;

		tag = ber_scanf( ber, "{iaa" /*}*/,
			&errcode, &ld->ld_matched, &ld->ld_error );

		if( tag == LBER_ERROR ) {
			ber_free( ber, 0 );
			ld->ld_errno = LDAP_DECODING_ERROR;
			return ld->ld_errno;
		}

		tag = ber_peek_tag(ber, &len);

		if( tag == LDAP_TAG_REFERRAL ) {
			/* skip 'em */
			if( ber_scanf( ber, "x" ) == LBER_ERROR ) {
				ber_free( ber, 0 );
				ld->ld_errno = LDAP_DECODING_ERROR;
				return ld->ld_errno;
			}

			tag = ber_peek_tag(ber, &len);
		}

		if( tag == LDAP_TAG_SASL_RES_CREDS ) {
			if( ber_scanf( ber, "O", &scred ) == LBER_ERROR ) {
				ber_free( ber, 0 );
				ld->ld_errno = LDAP_DECODING_ERROR;
				return ld->ld_errno;
			}
		}
	}

	ber_free( ber, 0 );

	if ( servercredp != NULL ) {
		*servercredp = scred;

	} else if ( scred != NULL ) {
		ber_bvfree( scred );
	}

	ld->ld_errno = errcode;

	if ( freeit ) {
		ldap_msgfree( res );
	}

	return( ld->ld_errno );
}

#ifdef HAVE_CYRUS_SASL
/*
* Various Cyrus SASL related stuff.
*/

#define MAX_BUFF_SIZE	65536
#define MIN_BUFF_SIZE	4096

static char *
array2str( char **a )
{
	char *s, **v, *p;
	int len = 0;

	for ( v = a; *v != NULL; v++ ) {
		len += strlen( *v ) + 1; /* for a space */
	}

	if ( len == 0 ) {
		return NULL;
	}

	s = LDAP_MALLOC ( len ); /* last space holds \0 */

	if ( s == NULL ) {
		return NULL;	
	}

	p = s;
	for ( v = a; *v != NULL; v++ ) {
		int len;

		if ( v != a ) {
			strncpy( p, " ", 1 );
			++p;
		}
		len = strlen( *v );
		strncpy( p, *v, len );
		p += len;
	}

	*p = '\0';

	return s;
}

int ldap_pvt_sasl_init( void )
{
	/* XXX not threadsafe */
	static int sasl_initialized = 0;

	if ( sasl_initialized ) {
		return 0;
	}
#ifndef CSRIMALLOC
	sasl_set_alloc( ber_memalloc, ber_memcalloc, ber_memrealloc, ber_memfree );
#endif /* CSRIMALLOC */

	if ( sasl_client_init( NULL ) == SASL_OK ) {
		sasl_initialized = 1;
		return 0;
	}

	return -1;
}

/*
 * SASL encryption support for LBER Sockbufs
 */

struct sb_sasl_data {
	sasl_conn_t		*sasl_context;
	Sockbuf_Buf		sec_buf_in;
	Sockbuf_Buf		buf_in;
	Sockbuf_Buf		buf_out;
};

static int
sb_sasl_setup( Sockbuf_IO_Desc *sbiod, void *arg )
{
	struct sb_sasl_data	*p;

	assert( sbiod != NULL );

	p = LBER_MALLOC( sizeof( *p ) );
	if ( p == NULL )
		return -1;
	p->sasl_context = (sasl_conn_t *)arg;
	ber_pvt_sb_buf_init( &p->sec_buf_in );
	ber_pvt_sb_buf_init( &p->buf_in );
	ber_pvt_sb_buf_init( &p->buf_out );
	if ( ber_pvt_sb_grow_buffer( &p->sec_buf_in, MIN_BUFF_SIZE ) < 0 ) {
		errno = ENOMEM;
		return -1;
	}

	sbiod->sbiod_pvt = p;

	return 0;
}

static int
sb_sasl_remove( Sockbuf_IO_Desc *sbiod )
{
	struct sb_sasl_data	*p;

	assert( sbiod != NULL );
	
	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;
	ber_pvt_sb_buf_destroy( &p->sec_buf_in );
	ber_pvt_sb_buf_destroy( &p->buf_in );
	ber_pvt_sb_buf_destroy( &p->buf_out );
	LBER_FREE( p );
	sbiod->sbiod_pvt = NULL;
	return 0;
}

static ber_len_t
sb_sasl_pkt_length( const char *buf, int debuglevel )
{
	ber_len_t		size;
	long			tmp;

	assert( buf != NULL );

	tmp = *((long *)buf);
	size = ntohl( tmp );
   
	if ( size > MAX_BUFF_SIZE ) {
		/* somebody is trying to mess me up. */
		ber_log_printf( LDAP_DEBUG_ANY, debuglevel,
			"sb_sasl_pkt_length: received illegal packet length "
			"of %lu bytes\n", (unsigned long)size );      
		size = 16; /* this should lead to an error. */
}

	return size + 4; /* include the size !!! */
}

/* Drop a processed packet from the input buffer */
static void
sb_sasl_drop_packet ( Sockbuf_Buf *sec_buf_in, int debuglevel )
{
	ber_slen_t			len;

	len = sec_buf_in->buf_ptr - sec_buf_in->buf_end;
	if ( len > 0 )
		memmove( sec_buf_in->buf_base, sec_buf_in->buf_base +
			sec_buf_in->buf_end, len );
   
	if ( len >= 4 ) {
		sec_buf_in->buf_end = sb_sasl_pkt_length( sec_buf_in->buf_base,
			debuglevel);
	}
	else {
		sec_buf_in->buf_end = 0;
	}
	sec_buf_in->buf_ptr = len;
}

static ber_slen_t
sb_sasl_read( Sockbuf_IO_Desc *sbiod, void *buf, ber_len_t len)
{
	struct sb_sasl_data	*p;
	ber_slen_t		ret, bufptr;
   
	assert( sbiod != NULL );
	assert( SOCKBUF_VALID( sbiod->sbiod_sb ) );

	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;

	/* Are there anything left in the buffer? */
	ret = ber_pvt_sb_copy_out( &p->buf_in, buf, len );
	bufptr = ret;
	len -= ret;

	if ( len == 0 )
		return bufptr;

	ber_pvt_sb_buf_destroy( &p->buf_in );

	/* Read the length of the packet */
	while ( p->sec_buf_in.buf_ptr < 4 ) {
		ret = LBER_SBIOD_READ_NEXT( sbiod, p->sec_buf_in.buf_base,
			4 - p->sec_buf_in.buf_ptr );
#ifdef EINTR
		if ( ( ret < 0 ) && ( errno == EINTR ) )
			continue;
#endif
		if ( ret <= 0 )
			return ret;

		p->sec_buf_in.buf_ptr += ret;
	}

	/* The new packet always starts at p->sec_buf_in.buf_base */
	ret = sb_sasl_pkt_length( p->sec_buf_in.buf_base,
		sbiod->sbiod_sb->sb_debug );

	/* Grow the packet buffer if neccessary */
	if ( ( p->sec_buf_in.buf_size < ret ) && 
			ber_pvt_sb_grow_buffer( &p->sec_buf_in, ret ) < 0 ) {
		errno = ENOMEM;
		return -1;
	}
	p->sec_buf_in.buf_end = ret;

	/* Did we read the whole encrypted packet? */
	while ( p->sec_buf_in.buf_ptr < p->sec_buf_in.buf_end ) {
		/* No, we have got only a part of it */
		ret = p->sec_buf_in.buf_end - p->sec_buf_in.buf_ptr;

		ret = LBER_SBIOD_READ_NEXT( sbiod, p->sec_buf_in.buf_base +
			p->sec_buf_in.buf_ptr, ret );
#ifdef EINTR
		if ( ( ret < 0 ) && ( errno == EINTR ) )
			continue;
#endif
		if ( ret <= 0 )
			return ret;

		p->sec_buf_in.buf_ptr += ret;
   	}

	/* Decode the packet */
	ret = sasl_decode( p->sasl_context, p->sec_buf_in.buf_base,
		p->sec_buf_in.buf_end, &p->buf_in.buf_base,
		(unsigned *)&p->buf_in.buf_end );
	if ( ret != SASL_OK ) {
		ber_log_printf( LDAP_DEBUG_ANY, sbiod->sbiod_sb->sb_debug,
			"sb_sasl_read: failed to decode packet: %s\n",
			sasl_errstring( ret, NULL, NULL ) );
		sb_sasl_drop_packet( &p->sec_buf_in,
			sbiod->sbiod_sb->sb_debug );
		errno = EIO;
		return -1;
	}
	
	/* Drop the packet from the input buffer */
	sb_sasl_drop_packet( &p->sec_buf_in, sbiod->sbiod_sb->sb_debug );

	p->buf_in.buf_size = p->buf_in.buf_end;

	bufptr += ber_pvt_sb_copy_out( &p->buf_in, (char*) buf + bufptr, len );

	return bufptr;
}

static ber_slen_t
sb_sasl_write( Sockbuf_IO_Desc *sbiod, void *buf, ber_len_t len)
{
	struct sb_sasl_data	*p;
	int			ret;

	assert( sbiod != NULL );
	assert( SOCKBUF_VALID( sbiod->sbiod_sb ) );

	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;

	/* Are there anything left in the buffer? */
	if ( p->buf_out.buf_ptr != p->buf_out.buf_end ) {
		ret = ber_pvt_sb_do_write( sbiod, &p->buf_out );
		if ( ret <= 0 )
			return ret;
	}

	/* now encode the next packet. */
	ber_pvt_sb_buf_destroy( &p->buf_out );
	ret = sasl_encode( p->sasl_context, buf, len, &p->buf_out.buf_base,
		(unsigned *)&p->buf_out.buf_size );
	if ( ret != SASL_OK ) {
		ber_log_printf( LDAP_DEBUG_ANY, sbiod->sbiod_sb->sb_debug,
			"sb_sasl_write: failed to encode packet: %s\n",
			sasl_errstring( ret, NULL, NULL ) );
		return -1;
	}
	p->buf_out.buf_end = p->buf_out.buf_size;

	ret = ber_pvt_sb_do_write( sbiod, &p->buf_out );
	if ( ret <= 0 )
		return ret;
	return len;
}

static int
sb_sasl_ctrl( Sockbuf_IO_Desc *sbiod, int opt, void *arg )
{
	struct sb_sasl_data	*p;

	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;

	if ( opt == LBER_SB_OPT_DATA_READY ) {
		if ( p->buf_in.buf_ptr != p->buf_in.buf_end )
			return 1;
	}
	
	return LBER_SBIOD_CTRL_NEXT( sbiod, opt, arg );
}

Sockbuf_IO ldap_pvt_sockbuf_io_sasl =
{
	sb_sasl_setup,		/* sbi_setup */
	sb_sasl_remove,		/* sbi_remove */
	sb_sasl_ctrl,		/* sbi_ctrl */
	sb_sasl_read,		/* sbi_read */
	sb_sasl_write,		/* sbi_write */
	NULL			/* sbi_close */
};

int ldap_pvt_sasl_install( Sockbuf *sb, void *ctx_arg )
{
	/* don't install the stuff unless security has been negotiated */

	if ( !ber_sockbuf_ctrl( sb, LBER_SB_OPT_HAS_IO,
			&ldap_pvt_sockbuf_io_sasl ) )
		ber_sockbuf_add_io( sb, &ldap_pvt_sockbuf_io_sasl,
			LBER_SBIOD_LEVEL_APPLICATION, ctx_arg );

	return LDAP_SUCCESS;
}

static int
sasl_err2ldap( int saslerr )
{
	int rc;

	switch (saslerr) {
		case SASL_CONTINUE:
			rc = LDAP_MORE_RESULTS_TO_RETURN;
			break;
		case SASL_OK:
			rc = LDAP_SUCCESS;
			break;
		case SASL_FAIL:
			rc = LDAP_LOCAL_ERROR;
			break;
		case SASL_NOMEM:
			rc = LDAP_NO_MEMORY;
			break;
		case SASL_NOMECH:
			rc = LDAP_AUTH_UNKNOWN;
			break;
		case SASL_BADAUTH:
			rc = LDAP_AUTH_UNKNOWN;
			break;
		case SASL_NOAUTHZ:
			rc = LDAP_PARAM_ERROR;
			break;
		case SASL_TOOWEAK:
		case SASL_ENCRYPT:
			rc = LDAP_AUTH_UNKNOWN;
			break;
		default:
			rc = LDAP_LOCAL_ERROR;
			break;
	}

	assert( rc == LDAP_SUCCESS || LDAP_API_ERROR( rc ) );
	return rc;
}

int
ldap_pvt_sasl_getmechs ( LDAP *ld, char **pmechlist )
{
	/* we need to query the server for supported mechs anyway */
	LDAPMessage *res, *e;
	char *attrs[] = { "supportedSASLMechanisms", NULL };
	char **values, *mechlist;
	int rc;

	Debug( LDAP_DEBUG_TRACE, "ldap_pvt_sasl_getmech\n", 0, 0, 0 );

	rc = ldap_search_s( ld, NULL, LDAP_SCOPE_BASE,
		NULL, attrs, 0, &res );

	if ( rc != LDAP_SUCCESS ) {
		return ld->ld_errno;
	}
		
	e = ldap_first_entry( ld, res );
	if ( e == NULL ) {
		if ( ld->ld_errno == LDAP_SUCCESS ) {
			ld->ld_errno = LDAP_UNAVAILABLE;
		}
		return ld->ld_errno;
	}

	values = ldap_get_values( ld, e, "supportedSASLMechanisms" );
	if ( values == NULL ) {
		ld->ld_errno = LDAP_NO_SUCH_ATTRIBUTE;
		ldap_msgfree( res );
		return ld->ld_errno;
	}

	mechlist = array2str( values );
	if ( mechlist == NULL ) {
		ld->ld_errno = LDAP_NO_MEMORY;
		LDAP_VFREE( values );
		ldap_msgfree( res );
		return ld->ld_errno;
	} 

	LDAP_VFREE( values );
	ldap_msgfree( res );

	*pmechlist = mechlist;

	return LDAP_SUCCESS;
}

int
ldap_pvt_sasl_bind(
	LDAP			*ld,
	LDAP_CONST char		*dn,
	LDAP_CONST char		*mechs,
	LDAP_CONST sasl_callback_t	*callbacks,
	LDAPControl		**sctrls,
	LDAPControl		**cctrls )
{
	const char *mech;
	int			saslrc, rc;
	sasl_ssf_t		*ssf = NULL;
	unsigned credlen;
	struct berval ccred, *scred;
	char *host;
	sasl_interact_t *client_interact = NULL;
	struct sockaddr_in	sin;
	socklen_t		len;
	sasl_security_properties_t	secprops;
	ber_socket_t		sd;

	Debug( LDAP_DEBUG_TRACE, "ldap_pvt_sasl_bind\n", 0, 0, 0 );

	/* do a quick !LDAPv3 check... ldap_sasl_bind will do the rest. */
	if (ld->ld_version < LDAP_VERSION3) {
		ld->ld_errno = LDAP_NOT_SUPPORTED;
		return ld->ld_errno;
	}

	ber_sockbuf_ctrl( ld->ld_sb, LBER_SB_OPT_GET_FD, &sd );

	if ( sd == AC_SOCKET_INVALID ) {
 		/* not connected yet */
 		int rc = ldap_open_defconn( ld );
  
		if( rc < 0 ) return ld->ld_errno;
		ber_sockbuf_ctrl( ld->ld_sb, LBER_SB_OPT_GET_FD, &sd );
	}   

	/* XXX this doesn't work with PF_LOCAL hosts */
	host = ldap_host_connected_to( ld->ld_sb );

	if ( host == NULL ) {
		ld->ld_errno = LDAP_UNAVAILABLE;
		return ld->ld_errno;
	}

	if ( ld->ld_sasl_context != NULL ) {
		sasl_dispose( &ld->ld_sasl_context );
	}

	saslrc = sasl_client_new( "ldap", host, callbacks, SASL_SECURITY_LAYER,
		&ld->ld_sasl_context );

	LDAP_FREE( host );

	if ( (saslrc != SASL_OK) && (saslrc != SASL_CONTINUE) ) {
		ld->ld_errno = sasl_err2ldap( saslrc );
		sasl_dispose( &ld->ld_sasl_context );
		return ld->ld_errno;
	}

	len = sizeof( sin );
	if ( getpeername( sd, (struct sockaddr *)&sin, &len ) == -1 ) {
		Debug( LDAP_DEBUG_ANY, "SASL: can't query remote IP.\n",
			0, 0, 0 );
		ld->ld_errno = LDAP_OPERATIONS_ERROR;
		return ld->ld_errno;
	}
	sasl_setprop( ld->ld_sasl_context, SASL_IP_REMOTE, &sin );

	len = sizeof( sin );
	if ( getsockname( sd, (struct sockaddr *)&sin, &len ) == -1 ) {
		Debug( LDAP_DEBUG_ANY, "SASL: can't query local IP.\n",
			0, 0, 0 );
		ld->ld_errno = LDAP_OPERATIONS_ERROR;
		return ld->ld_errno;
	}
	sasl_setprop( ld->ld_sasl_context, SASL_IP_LOCAL, &sin );

	memset( &secprops, '\0', sizeof( secprops ) );
	secprops.min_ssf = ld->ld_options.ldo_sasl_minssf;
	secprops.max_ssf = ld->ld_options.ldo_sasl_maxssf;
	secprops.security_flags = SASL_SECURITY_LAYER;
	secprops.maxbufsize = 65536;
	sasl_setprop( ld->ld_sasl_context, SASL_SEC_PROPS, &secprops );

	ccred.bv_val = NULL;
	ccred.bv_len = 0;

	saslrc = sasl_client_start( ld->ld_sasl_context,
		mechs,
		NULL,
		&client_interact,
		&ccred.bv_val,
		&credlen,
		&mech );

	ccred.bv_len = credlen;

	if ( (saslrc != SASL_OK) && (saslrc != SASL_CONTINUE) ) {
		ld->ld_errno = sasl_err2ldap( saslrc );
		sasl_dispose( &ld->ld_sasl_context );
		return ld->ld_errno;
	}

	scred = NULL;

	do {
		unsigned credlen;
		sasl_interact_t *client_interact = NULL;

		rc = ldap_sasl_bind_s( ld, dn, mech, &ccred, sctrls, cctrls, &scred );
		if ( rc == LDAP_SUCCESS ) {
			break;
		} else if ( rc != LDAP_SASL_BIND_IN_PROGRESS ) {
			if ( ccred.bv_val != NULL ) {
				LDAP_FREE( ccred.bv_val );
			}
			sasl_dispose( &ld->ld_sasl_context );
			return ld->ld_errno;
		}

		if ( ccred.bv_val != NULL ) {
			LDAP_FREE( ccred.bv_val );
			ccred.bv_val = NULL;
		}

		saslrc = sasl_client_step( ld->ld_sasl_context,
			(scred == NULL) ? NULL : scred->bv_val,
			(scred == NULL) ? 0 : scred->bv_len,
			&client_interact,
			&ccred.bv_val,
			&credlen );

		ccred.bv_len = credlen;
		ber_bvfree( scred );

		if ( (saslrc != SASL_OK) && (saslrc != SASL_CONTINUE) ) {
			ld->ld_errno = sasl_err2ldap( saslrc );
			sasl_dispose( &ld->ld_sasl_context );
			return ld->ld_errno;
		}
	} while ( rc == LDAP_SASL_BIND_IN_PROGRESS );

	assert ( rc == LDAP_SUCCESS );

	if ( sasl_getprop( ld->ld_sasl_context, SASL_SSF, (void **)&ssf )
		== SASL_OK && ssf && *ssf ) {
		ldap_pvt_sasl_install( ld->ld_sb, ld->ld_sasl_context );
	}

	return rc;
}

/* based on sample/sample-client.c */
static int
ldap_pvt_sasl_getsecret(sasl_conn_t *conn,
	void *context, int id, sasl_secret_t **psecret)
{
	struct berval *passphrase = (struct berval *)context;
	size_t len;           

	if ( conn == NULL || psecret == NULL || id != SASL_CB_PASS ) {
		return SASL_BADPARAM;
	}

	len = (passphrase != NULL) ? (size_t)passphrase->bv_len: 0;

	*psecret = (sasl_secret_t *) LDAP_MALLOC( sizeof( sasl_secret_t ) + len );
	if ( *psecret == NULL ) {
		return SASL_NOMEM;
	}

	(*psecret)->len = passphrase->bv_len;

	if ( passphrase != NULL ) {
		memcpy((*psecret)->data, passphrase->bv_val, len);
	}

	return SASL_OK;
}

static int
ldap_pvt_sasl_getsimple(void *context, int id, const char **result, int *len)
{
	const char *value = (const char *)context;

	if ( result == NULL ) {
		return SASL_BADPARAM;
	}

	switch ( id ) {
		case SASL_CB_USER:
		case SASL_CB_AUTHNAME:
			*result = value;
			if ( len )
				*len = value ? strlen( value ) : 0;
			break;
		case SASL_CB_LANGUAGE:
			*result = NULL;
			if ( len )
				*len = 0;
			break;
		default:
			return SASL_BADPARAM;
	}

	return SASL_OK;
}

int
ldap_pvt_sasl_get_option( LDAP *ld, int option, void *arg )
{
	sasl_ssf_t	*ssf;
	
	if ( ld == NULL )
		return -1;

	switch ( option ) {
		case LDAP_OPT_X_SASL_MINSSF:
			*(int *)arg = ld->ld_options.ldo_sasl_minssf;
			break;
		case LDAP_OPT_X_SASL_MAXSSF:
			*(int *)arg = ld->ld_options.ldo_sasl_maxssf;
			break;
		case LDAP_OPT_X_SASL_ACTSSF:
			if ( ld->ld_sasl_context == NULL ) {
				*(int *)arg = -1;
				break;
			}
			if ( sasl_getprop( ld->ld_sasl_context, SASL_SSF,
				(void **) &ssf ) != SASL_OK )
			{
				return -1;
			}
			*(int *)arg = *ssf;
			break;
		default:
			return -1;
	}
	return 0;
}

int
ldap_pvt_sasl_set_option( LDAP *ld, int option, void *arg )
{
	if ( ld == NULL )
		return -1;

	switch ( option ) {
		case LDAP_OPT_X_SASL_MINSSF:
			ld->ld_options.ldo_sasl_minssf = *(int *)arg;
			break;
		case LDAP_OPT_X_SASL_MAXSSF:
			ld->ld_options.ldo_sasl_maxssf = *(int *)arg;
			break;
		case LDAP_OPT_X_SASL_ACTSSF:
			/* This option is read-only */
		default:
			return -1;
	}
	return 0;
}

/*
 * ldap_negotiated_sasl_bind_s - bind to the ldap server (and X.500)
 * using SASL authentication.
 *
 * This routine attempts to authenticate the user referred by the
 * authentication id using the provided password.  An optional
 * authorization identity may be provided.  An DN is generally not
 * provided [see AuthMethod].
 *
 * If the mechanism negotiated does not require a password, the
 * passwd field is ignored.  [A callback mechanism should really
 * be used].
 * 
 * LDAP_SUCCESS is returned upon success, the ldap error code
 * otherwise.
 *
 * Examples:
 *	ldap_negotiated_sasl_bind_s( ld, NULL,
 *	    NULL, NULL, NULL,
 *		NULL, NULL, NULL, NULL );
 *
 *	ldap_negotiated_sasl_bind_s( ld, NULL,
 *	    "user@OPENLDAP.ORG", NULL, NULL,
 *		"GSSAPI", NULL, NULL, NULL );
 *
 *	ldap_negotiated_sasl_bind_s( ld, NULL,
 *	    "manager", "dn:cn=user,dc=openldap,dc=org", NULL,
 *		"DIGEST-MD5", NULL, NULL, NULL );
 *
 *	ldap_negotiated_sasl_bind_s( ld, NULL,
 *	    "root@OPENLDAP.ORG", "u:user@OPENLDAP.ORG", NULL,
 *		"GSSAPI", NULL, NULL, NULL );
 *
 *	ldap_negotiated_sasl_bind_s( ld, NULL,
 *	    "manager", "dn:cn=user,dc=openldap,dc=org", NULL,
 *		"DIGEST-MD5", NULL, NULL, NULL );
 */
int
ldap_negotiated_sasl_bind_s(
	LDAP *ld,
	LDAP_CONST char *dn, /* usually NULL */
	LDAP_CONST char *authenticationId,
	LDAP_CONST char *authorizationId, /* commonly NULL */
	LDAP_CONST char *saslMechanism,
	struct berval *passPhrase,
	LDAPControl **serverControls,
	LDAPControl **clientControls)
{
	int n;
	sasl_callback_t callbacks[4];
	int rc;

	Debug( LDAP_DEBUG_TRACE, "ldap_negotiated_sasl_bind_s\n", 0, 0, 0 );

	if( saslMechanism == NULL || *saslMechanism == '\0' ) {
		char *mechs;
		rc = ldap_pvt_sasl_getmechs( ld, &mechs );

		if( rc != LDAP_SUCCESS ) {
			return rc;
		}

		saslMechanism = mechs;
	}

	/* SASL Authentication Identity */
	callbacks[n=0].id = SASL_CB_AUTHNAME;
	callbacks[n].proc = ldap_pvt_sasl_getsimple;
	callbacks[n].context = (void *)authenticationId;

	/* SASL Authorization Identity (userid) */
	if( authorizationId != NULL ) {
		callbacks[++n].id = SASL_CB_USER;
		callbacks[n].proc = ldap_pvt_sasl_getsimple;
		callbacks[n].context = (void *)authorizationId;
	}

	callbacks[++n].id = SASL_CB_PASS;
	callbacks[n].proc = ldap_pvt_sasl_getsecret;
	callbacks[n].context = (void *)passPhrase;

	callbacks[++n].id = SASL_CB_LIST_END;
	callbacks[n].proc = NULL;
	callbacks[n].context = NULL;

	assert( n * sizeof(sasl_callback_t) < sizeof(callbacks) );

	rc = ldap_pvt_sasl_bind(ld, dn, saslMechanism, callbacks,
		serverControls, clientControls);

	return rc;
}
#endif /* HAVE_CYRUS_SASL */
