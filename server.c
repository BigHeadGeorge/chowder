#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <netdb.h>

#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "include/jsmn/jsmn.h"
#include "server.h"

char *mc_hash(size_t der_len, const uint8_t *der, const uint8_t secret[16]) {
	SHA_CTX c = {0};
	if (!SHA1_Init(&c)) {
		fprintf(stderr, "SHA1_init(): %lu\n", ERR_get_error());
		return NULL;
	}
	uint8_t server_id[20];
	for (int i = 0; i < 20; ++i) {
		server_id[i] = 32; // ASCII space
	}
	int success = 1;
	success &= SHA1_Update(&c, server_id, 20);
	success &= SHA1_Update(&c, secret, 16);
	success &= SHA1_Update(&c, der, der_len);
	if (!success) {
		fprintf(stderr, "SHA1_Update failed :(\n");
		return NULL;
	}
	uint8_t sum[SHA_DIGEST_LENGTH];
	if (!SHA1_Final(sum, &c)) {
		fprintf(stderr, "SHA1_Final(): %lu\n", ERR_get_error());
		return NULL;
	}

	BIGNUM *bn = BN_bin2bn(sum, SHA_DIGEST_LENGTH, NULL);
	int negative = BN_is_bit_set(bn, 159);
	if (negative) {
		const int len = BN_num_bytes(bn);
		unsigned char flipped[len];
		BN_bn2bin(bn, flipped);
		for (int i = 0; i < len; ++i)
			flipped[i] = ~flipped[i];
		BN_bin2bn(flipped, len, bn);
		BN_add_word(bn, 1);
	}
	char *hex = BN_bn2hex(bn);
	int hex_len = strlen(hex);
	char *hash = malloc(sizeof(char) * hex_len + 2);
	if (negative) {
		hash[0] = '-';
	}

	int index = negative;
	int num_found = 0;
	for (int i = 0; i < hex_len; ++i) {
		if (num_found || hex[i] != '0') {
			hash[index++] = hex[i];
			num_found = 1;
		}
	}
	hash[index] = 0;

	free(hex);
	BN_free(bn);
	return hash;
}

void ssl_cleanup(SSL_CTX *ctx, SSL *ssl) {
	SSL_free(ssl);
	SSL_CTX_free(ctx);
}

int ping_sessionserver(const char *username, const char *hash, char **response) {
	struct addrinfo hints = {0};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res;
	int err = getaddrinfo("sessionserver.mojang.com", "www", &hints, &res);
	if (err != 0) {
		fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(err));
		return -1;
	} else if (res == NULL) {
		fprintf(stderr, "no results found by getaddrinfo()\n");
		return -1;
	}
	int sfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sfd == -1) {
		perror("socket");
		return -1;
	}
	struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
	addr->sin_port = htons(443);
	if (connect(sfd, res->ai_addr, res->ai_addrlen) == -1) {
		perror("connect");
		return -1;
	}

	SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_method());
	if (ssl_ctx == NULL) {
		fprintf(stderr, "SSL_CTX_new(): %ld\n", ERR_get_error());
		return -1;
	}
	SSL *ssl = SSL_new(ssl_ctx);
	if (ssl == NULL) {
		fprintf(stderr, "SSL_new(): %ld\n", ERR_get_error());
		SSL_CTX_free(ssl_ctx);
		return -1;
	}
	if (!SSL_set_fd(ssl, sfd)) {
		fprintf(stderr, "SSL_set_fd(): %ld\n", ERR_get_error());
		ssl_cleanup(ssl_ctx, ssl);
		return -1;
	}

	err = SSL_connect(ssl);
	if (err <= 0) {
		fprintf(stderr, "SSL_connect(): %d\n", SSL_get_error(ssl, err));
		char err_str[256];
		ERR_error_string(ERR_get_error(), err_str);
		printf("error: %s\n", err_str);
		ssl_cleanup(ssl_ctx, ssl);
		return -1;
	}
	freeaddrinfo(res);

	const size_t buf_len = 2048;
	*response = malloc(sizeof(char) * buf_len);
	int write_len = snprintf(*response, buf_len, "GET /session/minecraft/hasJoined?username=%s&serverId=%s HTTP/1.1\r\nHost: sessionserver.mojang.com\r\nUser-Agent: Chowder :)\r\n\r\n", username, hash);
	err = SSL_write(ssl, *response, write_len);
	if (err <= 0) {
		fprintf(stderr, "SSL_write(): %d\n", SSL_get_error(ssl, err));
		ssl_cleanup(ssl_ctx, ssl);
		return -1;
	} else if (err != write_len) {
		fprintf(stderr, "write mismatch! %d != %d\n", err, write_len);
		ssl_cleanup(ssl_ctx, ssl);
		return -1;
	}
	memset(*response, 0, write_len);

	err = SSL_read(ssl, *response, buf_len);
	if (err <= 0) {
		fprintf(stderr, "SSL_read(): %d\n", SSL_get_error(ssl, err));
		ssl_cleanup(ssl_ctx, ssl);
		return -1;
	}

	SSL_shutdown(ssl);
	ssl_cleanup(ssl_ctx, ssl);
	close(sfd);
	return err;
}

int read_body(size_t resp_len, char *resp) {
	char *body_start;
	for (body_start = resp; body_start - resp < resp_len; ++body_start) {
		// TODO: kinda hacky
		//       probably check the request status + actually parse Content-Length for more safety
		if (*body_start == '\r' && !strncmp(body_start, "\r\n\r\n", 4)) {
			body_start += 4;
			break;
		}
	}
	if (body_start - resp == resp_len)
		return -1;

	size_t body_len = resp_len - (body_start - resp) + 1;
	return snprintf(resp, body_len, "%s", body_start);
}

int player_id(const char *username, const char *hash, char id[37]) {
	char *response;
	int response_len = ping_sessionserver(username, hash, &response);
	if (response_len < 0) {
		return -1;
	}
	size_t body_len = read_body((size_t) response_len, response);

	// parse player id
	jsmn_parser p;
	jsmntok_t t[128];
	jsmn_init(&p);
	int tokens = jsmn_parse(&p, response, body_len, t, 128);
	if (tokens < 0) {
		fprintf(stderr, "jsmn_parse() failed: %d\n", tokens);
		return -1;
	} else if (tokens < 1 || t[0].type != JSMN_OBJECT) {
		fprintf(stderr, "no JSON object\n");
		return -1;
	}
	char temp_id[32];
	for (int i = 1; i < tokens; ++i) {
		if (t[i].type == JSMN_STRING && t[i].end - t[i].start == 2 && !strncmp(response + t[i].start, "id", 2)) {
			for (int j = 0; j < 32; ++j)
				temp_id[j] = response[j + t[i+1].start];
			break;
		}
	}
	free(response);
	if (temp_id[0] == 0) {
		fprintf(stderr, "no \"id\" field present in sessionserver response\n");
		return -1;
	}

	// format id like a UUID (kinda dumb but it works)
	strncat(id, temp_id, 8);
	id[8] = '-';
	strncat(id, temp_id+8, 4);
	id[13] = '-';
	strncat(id, temp_id+12, 4);
	id[18] = '-';
	strncat(id, temp_id+16, 4);
	id[23] = '-';
	strncat(id, temp_id+20, 12);

	return 0;
}
