/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2016-2018 Daniel Lenski
 *
 * Author: Dan Lenski <dlenski@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <config.h>

#include "openconnect-internal.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <ctype.h>
#include <errno.h>

struct login_context {
	char *username;				/* Username that has already succeeded in some form */
	char *alt_secret;			/* Alternative secret (DO NOT FREE) */
	char *portal_userauthcookie;		/* portal-userauthcookie (from global-protect/getconfig.esp) */
	char *portal_prelogonuserauthcookie;	/* portal-prelogonuserauthcookie (from global-protect/getconfig.esp) */
	struct oc_auth_form *form;
};

void gpst_common_headers(struct openconnect_info *vpninfo,
			 struct oc_text_buf *buf)
{
	char *orig_ua = vpninfo->useragent;

	/* XX: more recent servers don't appear to require this specific UA value,
	 * but we don't have any good way to detect them.
	 */
	vpninfo->useragent = (char *)"PAN GlobalProtect";
	http_common_headers(vpninfo, buf);
	vpninfo->useragent = orig_ua;
}

/* Translate platform names (derived from AnyConnect) into the values
 * known to be emitted by GlobalProtect clients.
 */
const char *gpst_os_name(struct openconnect_info *vpninfo)
{
	if (!strcmp(vpninfo->platname, "mac-intel") || !strcmp(vpninfo->platname, "apple-ios"))
		return "Mac";
	else if (!strcmp(vpninfo->platname, "linux-64") || !strcmp(vpninfo->platname, "linux") || !strcmp(vpninfo->platname, "android"))
		return "Linux";
	else
		return "Windows";
}


/* Parse pre-login response ({POST,GET} /{global-protect,ssl-vpn}/pre-login.esp)
 *
 * Extracts the relevant arguments from the XML (username-label, password-label)
 * and uses them to build an auth form, which always has 2-3 fields:
 *
 *   1) username (hidden in challenge forms, since it's simply repeated)
 *   2) one secret value:
 *       - normal account password
 *       - "challenge" (2FA) password
 *       - cookie from external authentication flow ("alternative secret" INSTEAD OF password)
 *   3) inputStr for challenge form (shoehorned into form->action)
 *
 */
static int parse_prelogin_xml(struct openconnect_info *vpninfo, xmlNode *xml_node, void *cb_data)
{
	struct login_context *ctx = cb_data;
	struct oc_auth_form *form = NULL;
	struct oc_form_opt *opt, *opt2;
	char *prompt = NULL, *username_label = NULL, *password_label = NULL;
	char *saml_method = NULL, *saml_path = NULL;
	int result = 0;

	if (!xmlnode_is_named(xml_node, "prelogin-response"))
		goto out;

	for (xml_node = xml_node->children; xml_node; xml_node = xml_node->next) {
		char *s = NULL;
		if (!xmlnode_get_val(xml_node, "saml-request", &s)) {
			int len;
			free(saml_path);
			saml_path = openconnect_base64_decode(&len, s);
			if (len < 0) {
				vpn_progress(vpninfo, PRG_ERR, "Could not decode SAML request as base64: %s\n", s);
				free(s);
				result = -EINVAL;
				goto out;
			}
			free(s);
			realloc_inplace(saml_path, len+1);
			if (!saml_path) {
				result = -ENOMEM;
				goto out;
			}
			saml_path[len] = '\0';
		} else {
			xmlnode_get_val(xml_node, "saml-auth-method", &saml_method);
			xmlnode_get_val(xml_node, "authentication-message", &prompt);
			xmlnode_get_val(xml_node, "username-label", &username_label);
			xmlnode_get_val(xml_node, "password-label", &password_label);
			/* XX: should we save the certificate username from <ccusername/> ? */
		}
	}

	/* XX: Alt-secret form field must be specified for SAML, because we can't autodetect it */
	if (saml_method || saml_path) {
		if (ctx->portal_userauthcookie)
			vpn_progress(vpninfo, PRG_DEBUG, _("SAML authentication required; using portal-userauthcookie to continue SAML.\n"));
		else if (ctx->portal_prelogonuserauthcookie)
			vpn_progress(vpninfo, PRG_DEBUG, _("SAML authentication required; using portal-prelogonuserauthcookie to continue SAML.\n"));
		else if (ctx->alt_secret)
			vpn_progress(vpninfo, PRG_DEBUG, _("Destination form field %s was specified; assuming SAML %s authentication is complete.\n"),
			             ctx->alt_secret, saml_method);
		else {
			if (saml_method && !strcmp(saml_method, "REDIRECT"))
				vpn_progress(vpninfo, PRG_ERR,
				             _("SAML %s authentication is required via %s\n"),
				             saml_method, saml_path);
			else
				vpn_progress(vpninfo, PRG_ERR,
				             _("SAML %s authentication is required via external script.\n"),
				             saml_method);
			vpn_progress(vpninfo, PRG_ERR,
			             _("When SAML authentication is complete, specify destination form field by appending :field_name to login URL.\n"));
			/* XX: EINVAL will lead to "failure to parse response", with unnecessary/confusing extra logging output */
			result = -EPERM;
			goto out;
		}
	}

	/* Replace old form */
	form = ctx->form = calloc(1, sizeof(*form));
	if (!form) {
	nomem:
		free_auth_form(form);
		result = -ENOMEM;
		goto out;
	}
	form->message = prompt ? : strdup(_("Please enter your username and password"));
	prompt = NULL;
	form->auth_id = strdup("_login");

	/* First field (username) */
	opt = form->opts = calloc(1, sizeof(*opt));
	if (!opt)
		goto nomem;
	opt->name = strdup("user");
	if (asprintf(&opt->label, "%s: ", username_label ? : _("Username")) == 0)
		goto nomem;
	if (!ctx->username)
		opt->type = OC_FORM_OPT_TEXT;
	else {
		opt->type = OC_FORM_OPT_HIDDEN;
		opt->_value = ctx->username;
		ctx->username = NULL;
	}

	/* Second field (secret) */
	opt2 = opt->next = calloc(1, sizeof(*opt));
	if (!opt2)
		goto nomem;
	opt2->name = strdup(ctx->alt_secret ? : "passwd");
	if (asprintf(&opt2->label, "%s: ", ctx->alt_secret ? : password_label ? : _("Password")) == 0)
		goto nomem;

	/* XX: Some VPNs use a password in the first form, followed by a
	 * a token in the second ("challenge") form. Others use only a
	 * token. How can we distinguish these?
	 *
	 * Currently using the heuristic that a non-default label for the
	 * password in the first form means we should treat the first
	 * form's password as a token field.
	 */
	if (!can_gen_tokencode(vpninfo, form, opt2) && !ctx->alt_secret
	    && password_label && strcmp(password_label, "Password"))
		opt2->type = OC_FORM_OPT_TOKEN;
	else
		opt2->type = OC_FORM_OPT_PASSWORD;

	vpn_progress(vpninfo, PRG_TRACE, "Prelogin form %s: \"%s\" %s(%s)=%s, \"%s\" %s(%s)\n",
	             form->auth_id,
	             opt->label, opt->name, opt->type == OC_FORM_OPT_TEXT ? "TEXT" : "HIDDEN", opt->_value,
	             opt2->label, opt2->name, opt2->type == OC_FORM_OPT_PASSWORD ? "PASSWORD" : "TOKEN");

out:
	free(prompt);
	free(username_label);
	free(password_label);
	free(saml_method);
	free(saml_path);
	return result;
}

/* Callback function to create a new form from a challenge
 *
 */
static int challenge_cb(struct openconnect_info *vpninfo, char *prompt, char *inputStr, void *cb_data)
{
	struct login_context *ctx = cb_data;
	struct oc_auth_form *form = ctx->form;
	struct oc_form_opt *opt = form->opts, *opt2 = form->opts->next;

	/* Replace prompt, inputStr, and password prompt;
	 * clear password field, and make user field hidden.
	 */
	free(form->message);
	free(form->auth_id);
	free(form->action);
	free(opt2->label);
	free(opt2->_value);
	opt2->_value = NULL;
	opt->type = OC_FORM_OPT_HIDDEN;

	/* XX: Some VPNs use a password in the first form, followed by a
	 * a token in the second ("challenge") form. Others use only a
	 * token. How can we distinguish these?
	 *
	 * Currently using the heuristic that if the password field in
	 * the preceding form wasn't treated as a token field, treat this
	 * as a token field.
        */
	if (!can_gen_tokencode(vpninfo, form, opt2) && opt2->type == OC_FORM_OPT_PASSWORD)
		opt2->type = OC_FORM_OPT_TOKEN;
	else
		opt2->type = OC_FORM_OPT_PASSWORD;

	if (    !(form->message = strdup(prompt))
		 || !(form->action = strdup(inputStr))
		 || !(form->auth_id = strdup("_challenge"))
		 || !(opt2->label = strdup(_("Challenge: "))) )
		return -ENOMEM;

	vpn_progress(vpninfo, PRG_TRACE, "Challenge form %s: \"%s\" %s(%s)=%s, \"%s\" %s(%s), inputStr=%s\n",
	             form->auth_id,
	             opt->label, opt->name, opt->type == OC_FORM_OPT_TEXT ? "TEXT" : "HIDDEN", opt->_value,
	             opt2->label, opt2->name, opt2->type == OC_FORM_OPT_PASSWORD ? "PASSWORD" : "TOKEN",
	             inputStr);

	return -EAGAIN;
}

/* Parse gateway login response (POST /ssl-vpn/login.esp)
 *
 * Extracts the relevant arguments from the XML (<jnlp><application-desc><argument>...</argument></application-desc></jnlp>)
 * and uses them to build a query string fragment which is usable for subsequent requests.
 * This query string fragment is saved as vpninfo->cookie.
 *
 */
struct gp_login_arg {
	const char *opt;
	unsigned save:1;
	unsigned show:1;
	unsigned warn_missing:1;
	unsigned err_missing:1;
	unsigned unknown:1;
	const char *check;
};
static const struct gp_login_arg gp_login_args[] = {
	{ .unknown=1 },                                 /* seemingly always empty */
	{ .opt="authcookie", .save=1, .err_missing=1 },
	{ .opt="persistent-cookie", .warn_missing=1 },  /* 40 hex digits; persists across sessions */
	{ .opt="portal", .save=1, .warn_missing=1 },
	{ .opt="user", .save=1, .err_missing=1 },
	{ .opt="authentication-source", .show=1 },      /* LDAP-auth, AUTH-RADIUS_RSA_OTP, etc. */
	{ .opt="configuration", .warn_missing=1 },      /* usually vsys1 (sometimes vsys2, etc.) */
	{ .opt="domain", .save=1, .warn_missing=1 },
	{ .unknown=1 },                                 /* 4 arguments, seemingly always empty */
	{ .unknown=1 },
	{ .unknown=1 },
	{ .unknown=1 },
	{ .opt="connection-type", .err_missing=1, .check="tunnel" },
	{ .opt="password-expiration-days", .show=1 },   /* days until password expires, if not -1 */
	{ .opt="clientVer", .err_missing=1, .check="4100" },
	{ .opt="preferred-ip", .save=1 },
	{ .opt="portal-userauthcookie", .show=1},
	{ .opt="portal-prelogonuserauthcookie", .show=1},
	{ .opt="preferred-ipv6", .save=1 },
	{ .opt="usually-equals-4", .show=1 },           /* newer servers send "4" here, meaning unknown */
	{ .opt="usually-equals-unknown", .show=1 },     /* newer servers send "unknown" here */
};
static const int gp_login_nargs = (sizeof(gp_login_args)/sizeof(*gp_login_args));

static int parse_login_xml(struct openconnect_info *vpninfo, xmlNode *xml_node, void *cb_data)
{
	struct oc_text_buf *cookie = buf_alloc();
	char *value = NULL;
	const struct gp_login_arg *arg;
	int argn, unknown_args = 0, fatal_args = 0;

	if (!xmlnode_is_named(xml_node, "jnlp"))
		goto err_out;

	xml_node = xml_node->children;
	while (xml_node && xml_node->type != XML_ELEMENT_NODE)
		xml_node = xml_node->next;

	if (!xml_node || !xmlnode_is_named(xml_node, "application-desc"))
		goto err_out;

	xml_node = xml_node->children;
	/* XXX: Loop as long as there are EITHER more known arguments OR more XML tags,
	 * so that we catch both more-than-expected and fewer-than-expected arguments. */
	for (argn = 0; argn < gp_login_nargs || xml_node; argn++) {
		while (xml_node && xml_node->type != XML_ELEMENT_NODE)
			xml_node = xml_node->next;

		/* XX: argument 0 is unknown so we reuse this for extra arguments */
		arg = &gp_login_args[(argn < gp_login_nargs) ? argn : 0];

		if (!xml_node)
			value = NULL;
		else if (!xmlnode_get_val(xml_node, "argument", &value)) {
			if (value && (!value[0] || !strcmp(value, "(null)") || !strcmp(value, "-1"))) {
				free(value);
				value = NULL;
			} else if (arg->save) {
				/* XX: Some of the fields returned here (e.g. portal-*cookie) should NOT be
				 * URL-decoded in order to be reused correctly, but the ones which get saved
				 * into "cookie" must be URL-decoded. They will be needed for the (stupidly
				 * redundant) logout parameters. In particular the domain value "%28empty_domain%29"
				 * appears frequently in the wild, and it needs to be decoded here for the logout
				 * request to succeed.
				 */
				urldecode_inplace(value);
			}
			xml_node = xml_node->next;
		} else
			goto err_out;

		if (arg->unknown && value) {
			unknown_args++;
			vpn_progress(vpninfo, PRG_ERR,
						 _("GlobalProtect login returned unexpected argument value arg[%d]=%s\n"),
						 argn, value);
		} else if (arg->check && (!value || strcmp(value, arg->check))) {
			unknown_args++;
			fatal_args += arg->err_missing;
			vpn_progress(vpninfo, PRG_ERR,
			             _("GlobalProtect login returned %s=%s (expected %s)\n"),
			             arg->opt, value, arg->check);
		} else if ((arg->err_missing || arg->warn_missing) && !value) {
			unknown_args++;
			fatal_args += arg->err_missing;
			vpn_progress(vpninfo, PRG_ERR,
			             _("GlobalProtect login returned empty or missing %s\n"),
			             arg->opt);
		} else if (value && arg->show) {
			vpn_progress(vpninfo, PRG_INFO,
			             _("GlobalProtect login returned %s=%s\n"),
			             arg->opt, value);
		}

		if (value && arg->save)
			append_opt(cookie, arg->opt, value);

		free(value);
		value = NULL;
	}
	append_opt(cookie, "computer", vpninfo->localname);

	if (unknown_args)
		vpn_progress(vpninfo, PRG_ERR,
					 _("Please report %d unexpected values above (of which %d fatal) to <openconnect-devel@lists.infradead.org>\n"),
					 unknown_args, fatal_args);
	if (fatal_args) {
		buf_free(cookie);
		return -EPERM;
	}

	if (!buf_error(cookie)) {
		vpninfo->cookie = cookie->data;
		cookie->data = NULL;
	}
	return buf_free(cookie);

err_out:
	free(value);
	buf_free(cookie);
	return -EINVAL;
}

/* Parse portal login/config response (POST /ssl-vpn/getconfig.esp)
 *
 * Extracts the list of gateways from the XML, writes them to the XML config,
 * presents the user with a form to choose the gateway, and redirects
 * to that gateway.
 *
 */
static int parse_portal_xml(struct openconnect_info *vpninfo, xmlNode *xml_node, void *cb_data)
{
	struct login_context *ctx = cb_data;
	struct oc_auth_form *form;
	xmlNode *x, *x2, *x3, *gateways = NULL;
	struct oc_form_opt_select *opt;
	struct oc_text_buf *buf = NULL;
	int max_choices = 0, result;
	char *portal = NULL;
	char *hip_interval = NULL;

	form = calloc(1, sizeof(*form));
	if (!form)
		return -ENOMEM;

	form->message = strdup(_("Please select GlobalProtect gateway."));
	form->auth_id = strdup("_portal");

	opt = form->authgroup_opt = calloc(1, sizeof(*opt));
	if (!opt) {
		result = -ENOMEM;
		goto out;
	}
	opt->form.type = OC_FORM_OPT_SELECT;
	opt->form.name = strdup("gateway");
	opt->form.label = strdup(_("GATEWAY:"));
	form->opts = (void *)opt;

	/*
	 * The portal contains a ton of stuff, but basically none of it is
	 * useful to a VPN client that wishes to give control to the client
	 * user, as opposed to the VPN administrator.  The exceptions are the
	 * list of gateways in policy/gateways/external/list and the interval
	 * for HIP checks in policy/hip-collection/hip-report-interval
	 */
	if (xmlnode_is_named(xml_node, "policy")) {
		for (x = xml_node->children; x; x = x->next) {
			if (xmlnode_is_named(x, "gateways")) {
				for (x2 = x->children; x2; x2 = x2->next)
					if (xmlnode_is_named(x2, "external"))
						for (x3 = x2->children; x3; x3 = x3->next)
							if (xmlnode_is_named(x3, "list"))
								gateways = x3;
			} else if (xmlnode_is_named(x, "hip-collection")) {
				for (x2 = x->children; x2; x2 = x2->next) {
					if (!xmlnode_get_val(x2, "hip-report-interval", &hip_interval)) {
						int sec = atoi(hip_interval);
						if (vpninfo->trojan_interval)
							vpn_progress(vpninfo, PRG_INFO, _("Ignoring portal's HIP report interval (%d minutes), because interval is already set to %d minutes.\n"),
										 sec/60, vpninfo->trojan_interval/60);
						else {
							vpninfo->trojan_interval = sec - 60;
							vpn_progress(vpninfo, PRG_INFO, _("Portal set HIP report interval to %d minutes).\n"),
										 sec/60);
						}
					}
				}
			} else {
				xmlnode_get_val(x, "portal-name", &portal);
				if (!xmlnode_get_val(x, "portal-userauthcookie", &ctx->portal_userauthcookie)) {
					if (!*ctx->portal_userauthcookie || !strcmp(ctx->portal_userauthcookie, "empty")) {
						free(ctx->portal_userauthcookie);
						ctx->portal_userauthcookie = NULL;
					}
				}
				if (!xmlnode_get_val(x, "portal-prelogonuserauthcookie", &ctx->portal_prelogonuserauthcookie)) {
					if (!*ctx->portal_prelogonuserauthcookie || !strcmp(ctx->portal_prelogonuserauthcookie, "empty")) {
						free(ctx->portal_prelogonuserauthcookie);
						ctx->portal_prelogonuserauthcookie = NULL;
					}
				}
			}
		}
	}

	if (!gateways) {
		result = -EINVAL;
		goto out;
	}

	if (vpninfo->write_new_config) {
		buf = buf_alloc();
		buf_append(buf, "<GPPortal>\n  <ServerList>\n");
		if (portal) {
			buf_append(buf, "      <HostEntry><HostName>");
			buf_append_xmlescaped(buf, portal);
			buf_append(buf, "</HostName><HostAddress>%s", vpninfo->hostname);
			if (vpninfo->port!=443)
				buf_append(buf, ":%d", vpninfo->port);
			buf_append(buf, "/global-protect</HostAddress></HostEntry>\n");
		}
	}

	/* first, count the number of gateways */
	for (x = gateways->children; x; x = x->next)
		if (xmlnode_is_named(x, "entry"))
			max_choices++;

	opt->choices = calloc(max_choices, sizeof(opt->choices[0]));
	if (!opt->choices) {
		result = -ENOMEM;
		goto out;
	}

	/* each entry looks like <entry name="host[:443]"><description>Label</description></entry> */
	vpn_progress(vpninfo, PRG_INFO, _("%d gateway servers available:\n"), max_choices);
	for (x = gateways->children; x; x = x->next) {
		if (xmlnode_is_named(x, "entry")) {
			struct oc_choice *choice = calloc(1, sizeof(*choice));
			if (!choice) {
				result = -ENOMEM;
				goto out;
			}

			xmlnode_get_prop(x, "name", &choice->name);
			for (x2 = x->children; x2; x2=x2->next)
				if (!xmlnode_get_val(x2, "description", &choice->label)) {
					if (vpninfo->write_new_config) {
						buf_append(buf, "      <HostEntry><HostName>");
						buf_append_xmlescaped(buf, choice->label);
						buf_append(buf, "</HostName><HostAddress>%s/ssl-vpn</HostAddress></HostEntry>\n",
								   choice->name);
					}
				}

			opt->choices[opt->nr_choices++] = choice;
			vpn_progress(vpninfo, PRG_INFO, _("  %s (%s)\n"),
				     choice->label, choice->name);
		}
	}
	if (!opt->nr_choices) {
		vpn_progress(vpninfo, PRG_ERR,
					 _("GlobalProtect portal configuration lists no gateway servers.\n"));
		result = -EINVAL;
		goto out;
	}
	if (!vpninfo->authgroup && opt->nr_choices)
		vpninfo->authgroup = strdup(opt->choices[0]->name);

	if (vpninfo->write_new_config) {
		buf_append(buf, "  </ServerList>\n</GPPortal>\n");
		if ((result = buf_error(buf)))
			goto out;
		if ((result = vpninfo->write_new_config(vpninfo->cbdata, buf->data, buf->pos)))
			goto out;
	}

	/* process auth form to select gateway */
	result = process_auth_form(vpninfo, form);
	if (result == OC_FORM_RESULT_CANCELLED || result < 0)
		goto out;

	/* redirect to the gateway (no-op if it's the same host) */
	free(vpninfo->redirect_url);
	if (asprintf(&vpninfo->redirect_url, "https://%s", vpninfo->authgroup) == 0) {
		result = -ENOMEM;
		goto out;
	}
	result = handle_redirect(vpninfo);

out:
	buf_free(buf);
	free(portal);
	free(hip_interval);
	free_auth_form(form);
	return result;
}

/* Main login entry point
 *
 * portal: 0 for gateway login, 1 for portal login
 * alt_secret: "alternate secret" field (see new_auth_form)
 *
 */
static int gpst_login(struct openconnect_info *vpninfo, int portal, struct login_context *ctx)
{
	int result, blind_retry = 0;
	struct oc_text_buf *request_body = buf_alloc();
	const char *request_body_type = "application/x-www-form-urlencoded";
	char *xml_buf = NULL, *orig_path;

	/* Ask the user to fill in the auth form; repeat as necessary */
	for (;;) {
		int keep_urlpath = 0;
		if (vpninfo->urlpath) {
			/* XX: If the path ends with .esp (possibly followed by a query string), leave as-is */
			const char *esp = strstr(vpninfo->urlpath, ".esp");
			if (esp && (esp[4] == '\0' || esp[4] == '?'))
				keep_urlpath = 1;
		}
		if (!keep_urlpath) {
			orig_path = vpninfo->urlpath;
			if (asprintf(&vpninfo->urlpath, "%s/prelogin.esp?tmp=tmp&clientVer=4100&clientos=%s",
				     portal ? "global-protect" : "ssl-vpn", gpst_os_name(vpninfo)) < 0) {
				result = -ENOMEM;
				goto out;
			}
		}
		/* submit prelogin request to get form */
		result = do_https_request(vpninfo, "POST", NULL, NULL, &xml_buf, NULL, HTTP_REDIRECT);
		if (!keep_urlpath) {
			free(vpninfo->urlpath);
			vpninfo->urlpath = orig_path;
		}

		if (result >= 0)
			result = gpst_xml_or_error(vpninfo, xml_buf, parse_prelogin_xml, NULL, ctx);
		if (result)
			goto out;

	got_form:
		/* process auth form */
		result = process_auth_form(vpninfo, ctx->form);
		if (result)
			goto out;

	replay_form:
		/* generate token code if specified */
		result = do_gen_tokencode(vpninfo, ctx->form);
		if (result) {
			vpn_progress(vpninfo, PRG_ERR, _("Failed to generate OTP tokencode; disabling token\n"));
			vpninfo->token_bypassed = 1;
			goto out;
		}

		/* submit gateway login (ssl-vpn/login.esp) or portal config (global-protect/getconfig.esp) request */
		buf_truncate(request_body);
		buf_append(request_body, "jnlpReady=jnlpReady&ok=Login&direct=yes&clientVer=4100&prot=https:");
		append_opt(request_body, "ipv6-support", vpninfo->disable_ipv6 ? "no" : "yes");
		append_opt(request_body, "clientos", gpst_os_name(vpninfo));
		append_opt(request_body, "os-version", vpninfo->platname);
		append_opt(request_body, "server", vpninfo->hostname);
		append_opt(request_body, "computer", vpninfo->localname);
		if (ctx->portal_userauthcookie)
			append_opt(request_body, "portal-userauthcookie", ctx->portal_userauthcookie);
		if (ctx->portal_prelogonuserauthcookie)
			append_opt(request_body, "portal-prelogonuserauthcookie", ctx->portal_prelogonuserauthcookie);

		if (vpninfo->ip_info.addr)
			append_opt(request_body, "preferred-ip", vpninfo->ip_info.addr);
		if (vpninfo->ip_info.addr6)
			append_opt(request_body, "preferred-ipv6", vpninfo->ip_info.addr);
		if (ctx->form->action)
			append_opt(request_body, "inputStr", ctx->form->action);
		append_form_opts(vpninfo, ctx->form, request_body);
		if ((result = buf_error(request_body)))
			goto out;

		orig_path = vpninfo->urlpath;
		vpninfo->urlpath = strdup(portal ? "global-protect/getconfig.esp" : "ssl-vpn/login.esp");
		result = do_https_request(vpninfo, "POST", request_body_type, request_body, &xml_buf, NULL, HTTP_NO_FLAGS);
		free(vpninfo->urlpath);
		vpninfo->urlpath = orig_path;

		/* Result could be either a JavaScript challenge or XML */
		if (result >= 0)
			result = gpst_xml_or_error(vpninfo, xml_buf, portal ? parse_portal_xml : parse_login_xml,
									   challenge_cb, ctx);
		if (result == -EACCES) {
			/* Invalid username/password; reuse same form, but blank,
			 * unless we just did a blind retry.
			 */
			nuke_opt_values(ctx->form->opts);
			if (!blind_retry)
				goto got_form;
			else
				blind_retry = 0;
		} else {
			/* Save successful username */
			if (!ctx->username)
				ctx->username = strdup(ctx->form->opts->_value);
			if (result == -EAGAIN) {
				/* New form is already populated from the challenge */
				goto got_form;
			} else if (portal && result == 0) {
				/* Portal login succeeded; blindly retry same credentials on gateway if:
				 *      (a) we received a cookie that should allow automatic retry
				 *   OR (b) portal form was neither challenge auth nor alt-secret (SAML)
				 */
				portal = 0;
				if (ctx->portal_userauthcookie || ctx->portal_prelogonuserauthcookie ||
				    (strcmp(ctx->form->auth_id, "_challenge") && !ctx->alt_secret)) {
					blind_retry = 1;
					goto replay_form;
				}
			} else
				break;
		}
	}

out:
	buf_free(request_body);
	free(xml_buf);
	return result;
}

int gpst_obtain_cookie(struct openconnect_info *vpninfo)
{
	struct login_context ctx = { .username=NULL, .alt_secret=NULL, .portal_userauthcookie=NULL, .portal_prelogonuserauthcookie=NULL, .form=NULL };
	int result;

	/* An alternate password/secret field may be specified in the "URL path" (or --usergroup).
        * Known possibilities are:
	 *     /portal:portal-userauthcookie
	 *     /gateway:prelogin-cookie
	 */
	if (vpninfo->urlpath
	    && (ctx.alt_secret = strrchr(vpninfo->urlpath, ':')) != NULL) {
		*(ctx.alt_secret) = '\0';
		ctx.alt_secret = strdup(ctx.alt_secret+1);
	}

	if (vpninfo->urlpath && (!strcmp(vpninfo->urlpath, "portal") || !strncmp(vpninfo->urlpath, "global-protect", 14))) {
		/* assume the server is a portal */
		result = gpst_login(vpninfo, 1, &ctx);
	} else if (vpninfo->urlpath && (!strcmp(vpninfo->urlpath, "gateway") || !strncmp(vpninfo->urlpath, "ssl-vpn", 7))) {
		/* assume the server is a gateway */
		result = gpst_login(vpninfo, 0, &ctx);
	} else {
		/* first try handling it as a portal, then a gateway */
		result = gpst_login(vpninfo, 1, &ctx);
		if (result == -EEXIST) {
			result = gpst_login(vpninfo, 0, &ctx);
			if (result == -EEXIST)
				vpn_progress(vpninfo, PRG_ERR, _("Server is neither a GlobalProtect portal nor a gateway.\n"));
		}
	}
	free(ctx.username);
	free(ctx.alt_secret);
	free(ctx.portal_userauthcookie);
	free(ctx.portal_prelogonuserauthcookie);
	free_auth_form(ctx.form);
	return result;
}

int gpst_bye(struct openconnect_info *vpninfo, const char *reason)
{
	char *orig_path;
	int result;
	struct oc_text_buf *request_body = buf_alloc();
	const char *request_body_type = "application/x-www-form-urlencoded";
	const char *method = "POST";
	char *xml_buf = NULL;

	/* In order to logout successfully, the client must send not only
	 * the session's authcookie, but also the portal, user, computer,
	 * and domain matching the values sent with the getconfig request.
	 *
	 * You read that right: the client must send a bunch of irrelevant
	 * non-secret values in its logout request. If they're wrong or
	 * missing, the logout will fail and the authcookie will remain
	 * valid -- which is a security hole.
	 *
	 * Don't blame me. I didn't design this.
	 */
	buf_append(request_body, "%s", vpninfo->cookie);
	if ((result = buf_error(request_body)))
		goto out;

	/* We need to close and reopen the HTTPS connection (to kill
	 * the tunnel session) and submit a new HTTPS request to
	 * logout.
	 */
	orig_path = vpninfo->urlpath;
	vpninfo->urlpath = strdup("ssl-vpn/logout.esp");
	openconnect_close_https(vpninfo, 0);
	result = do_https_request(vpninfo, method, request_body_type, request_body, &xml_buf, NULL, HTTP_NO_FLAGS);
	free(vpninfo->urlpath);
	vpninfo->urlpath = orig_path;

	/* logout.esp returns HTTP status 200 and <response status="success"> when
	 * successful, and all manner of malformed junk when unsuccessful.
	 */
	if (result >= 0)
		result = gpst_xml_or_error(vpninfo, xml_buf, NULL, NULL, NULL);

	if (result < 0)
		vpn_progress(vpninfo, PRG_ERR, _("Logout failed.\n"));
	else
		vpn_progress(vpninfo, PRG_INFO, _("Logout successful.\n"));

out:
	buf_free(request_body);
	free(xml_buf);
	return result;
}