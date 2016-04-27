#include <Python.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/snmpv3_api.h>
#include <stdio.h>
#include <sys/types.h>
#ifdef I_SYS_TIME
#include <sys/time.h>
#endif
#include <netdb.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_REGEX_H
#include <regex.h>
#endif

/* include bitarray data structure for v1 queries */
#include "simple_bitarray.h"

/*
 * In snmpv1 when using retry_nosuch we need to track the
 * index of each bad OID in the responses using a bitarray;
 * DEFAULT_NUM_BAD_OIDS is a tradeoff to avoid allocating
 * heavily on the heap; if we need to track more, we can
 * just malloc on the heap.
 */
#define DEFAULT_NUM_BAD_OIDS (sizeof(bitarray) * 8 * 3)

#define STRLEN(x) ((x) ? strlen((x)) : 0)

#define SUCCESS (1)
#define FAILURE (0)

#define VARBIND_TAG_F (0)
#define VARBIND_IID_F (1)
#define VARBIND_VAL_F (2)
#define VARBIND_TYPE_F (3)

#define TYPE_UNKNOWN (0)
#define MAX_TYPE_NAME_LEN (32)
#define STR_BUF_SIZE ((MAX_TYPE_NAME_LEN) * (MAX_OID_LEN))
#define MAX_VALUE_SIZE (65536)
#define MAX_INVALID_OIDS (MAX_VALUE_SIZE / MIN_OID_LEN)
#define ENG_ID_BUF_SIZE (32)

#define NO_RETRY_NOSUCH (0)

#define USE_NUMERIC_OIDS (0x08)
#define NON_LEAF_NAME (0x04)
#define USE_LONG_NAMES (0x02)
#define FAIL_ON_NULL_IID (0x01)
#define NO_FLAGS (0x00)

#define SAFE_FREE(x)                                                           \
  do {                                                                         \
    if (x != NULL) {                                                           \
      free(x);                                                                 \
    }                                                                          \
  } while (0)

typedef netsnmp_session SnmpSession;

/*
 * This structure is attached to the easysnmp.Session
 * object as a Python Capsule (or CObject).
 *
 * This allows a one time allocation of large buffers
 * without resorting to (unnecessary) allocation on the
 * stack, but also remains thread safe; as long as only
 * one Session object is restricted to each thread.
 *
 * This is allocated in create_session_capsule()
 * and later (automatically via garbage collection) destroyed
 * delete_session_capsule().
 */
typedef struct {
  /*
   * Technically this should be a (void *), but this probably
   * won't ever change in Net-SNMP.
   */
  netsnmp_session *handle;

  /* err_str is used to fetch the error message from net-snmp libs */
  char err_str[STR_BUF_SIZE];
  int err_ind;
  int err_num;

  unsigned long snmp_version;
  int getlabel_flag;
  int sprintval_flag;
  int old_format;
  int best_guess;
  int retry_nosuch;
} session_capsule_ctx;

typedef struct {
  char *op_name;
  int getlabel_flag;

  netsnmp_pdu *pdu;
  netsnmp_pdu *response;
  netsnmp_variable_list *vars;

  oid **oid_arr;
  int *oid_arr_len;
  char **initial_oid_str_arr;
  char **oid_str_arr;
  char **oid_idx_str_arr;
  char *initial_oid;
  PyObject *varlist;
  PyObject *varbind;
  int varlist_len;

  int len;
  int type;
  char type_str[MAX_TYPE_NAME_LEN];
  u_char str_buf[STR_BUF_SIZE];

  int error;
} snmp_op_data;

static PyObject *create_session_capsule(SnmpSession *ss);
static void *get_session_context(PyObject *session);
static void delete_session_capsule(PyObject *session_capsule);
static void snmp_op_data_reset(snmp_op_data *data);
static int snmp_op_data_load(snmp_op_data *data, int best_guess);
static void snmp_op_data_finish(snmp_op_data *data);
static int send_pdu_request(session_capsule_ctx *session_ctx, snmp_op_data* data);
static PyObject *read_variable(netsnmp_variable_list *vars, snmp_op_data* data, int getlabel_flag, int sprintval_flag);

static int __is_numeric_oid(char *oidstr);
static int __is_leaf(struct tree *tp);
static int __translate_appl_type(char *typestr);
static int __translate_asn_type(int type);
static int __snprint_value(char *buf, size_t buf_len,
                           netsnmp_variable_list *var, struct tree *tp,
                           int type, int flag);
static int __sprint_num_objid(char *buf, oid *objid, int len);
static int __scan_num_objid(char *buf, oid *objid, size_t *len);
static int __get_type_str(int type, char *str, int log_error);
static int __get_label_iid(char *name, char **last_label, char **iid, int flag);
static struct tree *__tag2oid(char *tag, char *iid, oid *oid_arr,
                              int *oid_arr_len, int *type, int best_guess);
static int __concat_oid_str(oid *doid_arr, int *doid_arr_len, char *soid_str);
static int __add_var_val_str(netsnmp_pdu *pdu, oid *name, int name_length,
                             char *val, int len, int type);

static void py_log_msg(int log_level, char *printf_fmt, ...);

enum { INFO, WARNING, ERROR, DEBUG, EXCEPTION };

static PyObject *easysnmp_import = NULL;
static PyObject *easysnmp_exceptions_import = NULL;
static PyObject *easysnmp_compat_import = NULL;
static PyObject *logging_import = NULL;

static PyObject *PyLogger = NULL;
static PyObject *EasySNMPError = NULL;
static PyObject *EasySNMPConnectionError = NULL;
static PyObject *EasySNMPTimeoutError = NULL;
static PyObject *EasySNMPNoSuchNameError = NULL;
static PyObject *EasySNMPUnknownObjectIDError = NULL;
static PyObject *EasySNMPNoSuchObjectError = NULL;
static PyObject *EasySNMPNoSuchInstanceError = NULL;
static PyObject *EasySNMPUndeterminedTypeError = NULL;

/*
 * Ripped wholesale from library/tools.h from Net-SNMP 5.7.3
 * to remain compatible with versions 5.7.2 and earlier.
 */
static void *compat_netsnmp_memdup(const void *from, size_t size) {
  void *to = NULL;

  if (from) {
    to = malloc(size);
    if (to) {
      memcpy(to, from, size);
    }
  }
  return to;
}

void __libraries_init(char *appname) {
  static int have_inited = 0;

  if (have_inited) {
    return;
  }
  have_inited = 1;

  snmp_set_quick_print(1);

  /* completely disable logging otherwise it will default to stderr */
  netsnmp_register_loghandler(NETSNMP_LOGHANDLER_NONE, 0);

  init_snmp(appname);

  netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID,
                         NETSNMP_DS_LIB_DONT_BREAKDOWN_OIDS, 1);
  netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_PRINT_SUFFIX_ONLY,
                     1);
  netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_OID_OUTPUT_FORMAT,
                     NETSNMP_OID_OUTPUT_SUFFIX);
}

static int __is_numeric_oid(char *oidstr) {
  if (!oidstr) {
    return 0;
  }
  for (; *oidstr; oidstr++) {
    if (isalpha((int)*oidstr)) {
      return 0;
    }
  }
  return 1;
}

static int __is_leaf(struct tree *tp) {
  char buf[MAX_TYPE_NAME_LEN];
  return (tp && (__get_type_str(tp->type, buf, 0) ||
                 (tp->parent && __get_type_str(tp->parent->type, buf, 0))));
}

static int __translate_appl_type(char *typestr) {
  if (typestr == NULL || *typestr == '\0') {
    return TYPE_UNKNOWN;
  }

  /*
   * looking at a one-char string, so use snmpset(1)
   * type specification that allows for single characters
   * (note: 'd' for decimal string and 'x' for hex is missing)
   */
  if (typestr[1] == '\0') {
    switch (typestr[0]) {
    case 'i':
      return TYPE_INTEGER;
    case 'u':
      return TYPE_UNSIGNED32;
    case 's':
      return TYPE_OCTETSTR;
    case 'n':
      return TYPE_NULL;
    case 'o':
      return TYPE_OBJID;
    case 't':
      return TYPE_TIMETICKS;
    case 'a':
      return TYPE_IPADDR;
    case 'b':
      return TYPE_BITSTRING;
    default:
      return TYPE_UNKNOWN;
    }
  }

  if (!strncasecmp(typestr, "INTEGER32", 8)) {
    return TYPE_INTEGER32;
  }
  if (!strncasecmp(typestr, "INTEGER", 3)) {
    return TYPE_INTEGER;
  }
  if (!strncasecmp(typestr, "UNSIGNED32", 3)) {
    return TYPE_UNSIGNED32;
  }
  if (!strcasecmp(typestr, "COUNTER")) /* check all in case counter64 */
  {
    return TYPE_COUNTER;
  }
  if (!strncasecmp(typestr, "GAUGE", 3)) {
    return TYPE_GAUGE;
  }
  if (!strncasecmp(typestr, "IPADDR", 3)) {
    return TYPE_IPADDR;
  }
  if (!strncasecmp(typestr, "OCTETSTR", 3)) {
    return TYPE_OCTETSTR;
  }
  if (!strncasecmp(typestr, "TICKS", 3)) {
    return TYPE_TIMETICKS;
  }
  if (!strncasecmp(typestr, "OPAQUE", 3)) {
    return TYPE_OPAQUE;
  }
  if (!strncasecmp(typestr, "OBJECTID", 3)) {
    return TYPE_OBJID;
  }
  if (!strncasecmp(typestr, "NETADDR", 3)) {
    return TYPE_NETADDR;
  }
  if (!strncasecmp(typestr, "COUNTER64", 3)) {
    return TYPE_COUNTER64;
  }
  if (!strncasecmp(typestr, "NULL", 3)) {
    return TYPE_NULL;
  }
  if (!strncasecmp(typestr, "BITS", 3)) {
    return TYPE_BITSTRING;
  }
  if (!strncasecmp(typestr, "ENDOFMIBVIEW", 3)) {
    return SNMP_ENDOFMIBVIEW;
  }
  if (!strncasecmp(typestr, "NOSUCHOBJECT", 7)) {
    return SNMP_NOSUCHOBJECT;
  }
  if (!strncasecmp(typestr, "NOSUCHINSTANCE", 7)) {
    return SNMP_NOSUCHINSTANCE;
  }
  if (!strncasecmp(typestr, "UINTEGER", 3)) {
    return (TYPE_UINTEGER); /* historic - should not show up */
                            /* but it does?                  */
  }
  if (!strncasecmp(typestr, "NOTIF", 3)) {
    return TYPE_NOTIFTYPE;
  }
  if (!strncasecmp(typestr, "TRAP", 4)) {
    return TYPE_TRAPTYPE;
  }
  return TYPE_UNKNOWN;
}

static int __translate_asn_type(int type) {
  switch (type) {
  case ASN_INTEGER:
    return TYPE_INTEGER;
  case ASN_OCTET_STR:
    return TYPE_OCTETSTR;
  case ASN_OPAQUE:
    return TYPE_OPAQUE;
  case ASN_OBJECT_ID:
    return TYPE_OBJID;
  case ASN_TIMETICKS:
    return TYPE_TIMETICKS;
  case ASN_GAUGE:
    return TYPE_GAUGE;
  case ASN_COUNTER:
    return TYPE_COUNTER;
  case ASN_IPADDRESS:
    return TYPE_IPADDR;
  case ASN_BIT_STR:
    return TYPE_BITSTRING;
  case ASN_NULL:
    return TYPE_NULL;
  /* no translation for these exception type values */
  case SNMP_ENDOFMIBVIEW:
  case SNMP_NOSUCHOBJECT:
  case SNMP_NOSUCHINSTANCE:
    return type;
  case ASN_UINTEGER:
    return TYPE_UINTEGER;
  case ASN_COUNTER64:
    return TYPE_COUNTER64;
  default:
    py_log_msg(ERROR, "translate_asn_type: unhandled asn type (%d)", type);

    return TYPE_OTHER;
  }
}

#define USE_BASIC (0)
#define USE_ENUMS (1)
#define USE_SPRINT_VALUE (2)
static int __snprint_value(char *buf, size_t buf_len,
                           netsnmp_variable_list *var, struct tree *tp,
                           int type, int flag) {
  int len = 0;
  u_char *ip;
  struct enum_list *ep;

  buf[0] = '\0';
  if (flag == USE_SPRINT_VALUE) {
    snprint_value(buf, buf_len, var->name, var->name_length, var);
    len = STRLEN(buf);
  } else {
    switch (var->type) {
    case ASN_INTEGER:
      if (flag == USE_ENUMS) {
        for (ep = tp->enums; ep; ep = ep->next) {
          if (ep->value == *var->val.integer) {
            strlcpy(buf, ep->label, buf_len);
            len = STRLEN(buf);
            break;
          }
        }
      }
      if (!len) {
        snprintf(buf, buf_len, "%ld", *var->val.integer);
        len = STRLEN(buf);
      }
      break;

    case ASN_GAUGE:
    case ASN_COUNTER:
    case ASN_TIMETICKS:
    case ASN_UINTEGER:
      snprintf(buf, buf_len, "%lu", (unsigned long)*var->val.integer);
      len = STRLEN(buf);
      break;

    case ASN_OCTET_STR:
    case ASN_OPAQUE:
      len = var->val_len;
      if (len > buf_len) {
        len = buf_len;
      }
      memcpy(buf, (char *)var->val.string, len);
      break;

    case ASN_IPADDRESS:
      ip = (u_char *)var->val.string;
      snprintf(buf, buf_len, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
      len = STRLEN(buf);
      break;

    case ASN_NULL:
      break;

    case ASN_OBJECT_ID:
      __sprint_num_objid(buf, (oid *)(var->val.objid),
                         var->val_len / sizeof(oid));
      len = STRLEN(buf);
      break;

    case SNMP_ENDOFMIBVIEW:
      snprintf(buf, buf_len, "%s", "ENDOFMIBVIEW");
      len = STRLEN(buf);
      break;
    case SNMP_NOSUCHOBJECT:
      snprintf(buf, buf_len, "%s", "NOSUCHOBJECT");
      len = STRLEN(buf);
      break;
    case SNMP_NOSUCHINSTANCE:
      snprintf(buf, buf_len, "%s", "NOSUCHINSTANCE");
      len = STRLEN(buf);
      break;

    case ASN_COUNTER64:

#ifdef OPAQUE_SPECIAL_TYPES
    case ASN_OPAQUE_COUNTER64:
    case ASN_OPAQUE_U64:
#endif
      printU64(buf, (struct counter64 *)var->val.counter64);
      len = STRLEN(buf);
      break;

#ifdef OPAQUE_SPECIAL_TYPES
    case ASN_OPAQUE_I64:
      printI64(buf, (struct counter64 *)var->val.counter64);
      len = STRLEN(buf);
      break;
#endif

    case ASN_BIT_STR:
      snprint_bitstring(buf, buf_len, var, NULL, NULL, NULL);
      len = STRLEN(buf);
      break;

#ifdef OPAQUE_SPECIAL_TYPES
    case ASN_OPAQUE_FLOAT:
      if (var->val.floatVal) {
        snprintf(buf, buf_len, "%f", *var->val.floatVal);
      }
      break;

    case ASN_OPAQUE_DOUBLE:
      if (var->val.doubleVal) {
        snprintf(buf, buf_len, "%f", *var->val.doubleVal);
      }
      break;
#endif

    case ASN_NSAP:
    default:
      py_log_msg(ERROR, "snprint_value: asn type not handled %d", var->type);
    }
  }
  return len;
}

static int __sprint_num_objid(char *buf, oid *objid, int len) {
  int i;
  buf[0] = '\0';
  for (i = 0; i < len; i++) {
    sprintf(buf, ".%lu", *objid++);
    buf += STRLEN(buf);
  }
  return SUCCESS;
}

static int __scan_num_objid(char *buf, oid *objid, size_t *len) {
  char *cp;
  *len = 0;
  if (*buf == '.') {
    buf++;
  }
  cp = buf;
  while (*buf) {
    if (*buf++ == '.') {
      sscanf(cp, "%lu", objid++);
      /* *objid++ = atoi(cp); */
      (*len)++;
      cp = buf;
    } else {
      if (isalpha((int)*buf)) {
        return FAILURE;
      }
    }
  }
  sscanf(cp, "%lu", objid++);
  /* *objid++ = atoi(cp); */
  (*len)++;
  return SUCCESS;
}

static int __get_type_str(int type, char *str, int log_error) {
  switch (type) {
  case TYPE_OBJID:
    strcpy(str, "OBJECTID");
    break;
  case TYPE_OCTETSTR:
    strcpy(str, "OCTETSTR");
    break;
  case TYPE_INTEGER:
    strcpy(str, "INTEGER");
    break;
  case TYPE_INTEGER32:
    strcpy(str, "INTEGER32");
    break;
  case TYPE_UNSIGNED32:
    strcpy(str, "UNSIGNED32");
    break;
  case TYPE_NETADDR:
    strcpy(str, "NETADDR");
    break;
  case TYPE_IPADDR:
    strcpy(str, "IPADDR");
    break;
  case TYPE_COUNTER:
    strcpy(str, "COUNTER");
    break;
  case TYPE_GAUGE:
    strcpy(str, "GAUGE");
    break;
  case TYPE_TIMETICKS:
    strcpy(str, "TICKS");
    break;
  case TYPE_OPAQUE:
    strcpy(str, "OPAQUE");
    break;
  case TYPE_COUNTER64:
    strcpy(str, "COUNTER64");
    break;
  case TYPE_NULL:
    strcpy(str, "NULL");
    break;
  case SNMP_ENDOFMIBVIEW:
    strcpy(str, "ENDOFMIBVIEW");
    break;
  case SNMP_NOSUCHOBJECT:
    strcpy(str, "NOSUCHOBJECT");
    break;
  case SNMP_NOSUCHINSTANCE:
    strcpy(str, "NOSUCHINSTANCE");
    break;
  case TYPE_UINTEGER:
    strcpy(str, "UINTEGER"); /* historic - should not show up */
    /* but it does?                  */
    break;
  case TYPE_NOTIFTYPE:
    strcpy(str, "NOTIF");
    break;
  case TYPE_BITSTRING:
    strcpy(str, "BITS");
    break;
  case TYPE_TRAPTYPE:
    strcpy(str, "TRAP");
    break;
  case TYPE_OTHER: /* not sure if this is a valid leaf type?? */
  case TYPE_NSAPADDRESS:
  default: /* unsupported types for now */
    strcpy(str, "");
    if (log_error) {
      py_log_msg(ERROR, "unspported type found: %d", type);
    }
    return FAILURE;
  }
  return SUCCESS;
}

/* does a destructive disection of <label1>...<labeln>.<iid> returning
   <labeln> and <iid> in seperate strings (note: will destructively
   alter input string, 'name') */
static int __get_label_iid(char *name, char **last_label, char **iid,
                           int flag) {
  char *lcp;
  char *icp;
  int len = STRLEN(name);
  int found_label = 0;

  *last_label = *iid = NULL;

  if (len == 0) {
    return FAILURE;
  }

  /* Handle case where numeric oid's have been requested.  The input 'name'
  ** in this case should be a numeric OID -- return failure if not.
  */
  if ((flag & USE_NUMERIC_OIDS)) {
    if (!__is_numeric_oid(name)) {
      return FAILURE;
    }

    /* Walk backward through the string, looking for first two '.' chars */
    lcp = &(name[len]);
    icp = NULL;
    while (lcp > name) {
      if (*lcp == '.') {
        /* If this is the first occurence of '.', note it in icp.
        ** Otherwise, this must be the second occurrence, so break
        ** out of the loop.
        */
        if (icp == NULL) {
          icp = lcp;
        } else {
          break;
        }
      }
      lcp--;
    }

    /* Make sure we found at least a label and index. */
    if (!icp) {
      return FAILURE;
    }

    /* Push forward past leading '.' chars and separate the strings. */
    lcp++;
    *icp++ = '\0';

    *last_label = (flag & USE_LONG_NAMES) ? name : lcp;
    *iid = icp;

    return SUCCESS;
  }

  lcp = icp = &(name[len]);

  while (lcp > name) {
    if (*lcp == '.') {
      if (found_label) {
        lcp++;
        break;
      } else {
        icp = lcp;
      }
    }
    if (!found_label && isalpha((int)*lcp)) {
      found_label = 1;
    }
    lcp--;
  }

  if (!found_label ||
      (!isdigit((int)*(icp + 1)) && (flag & FAIL_ON_NULL_IID))) {
    return FAILURE;
  }

  if (flag & NON_LEAF_NAME) /* dont know where to start instance id */
  {
    /* put the whole thing in label */
    icp = &(name[len]);
    flag |= USE_LONG_NAMES;
    /* special hack in case no mib loaded - object identifiers will
    * start with .iso.<num>.<num>...., in which case it is preferable
    * to make the label entirely numeric (i.e., convert "iso" => "1")
    */
    if (*lcp == '.' && lcp == name) {
      if (!strncmp(".ccitt.", lcp, 7)) {
        name += 2;
        *name = '.';
        *(name + 1) = '0';
      } else if (!strncmp(".iso.", lcp, 5)) {
        name += 2;
        *name = '.';
        *(name + 1) = '1';
      } else if (!strncmp(".joint-iso-ccitt.", lcp, 17)) {
        name += 2;
        *name = '.';
        *(name + 1) = '2';
      }
    }
  } else if (*icp) {
    *(icp++) = '\0';
  }
  *last_label = (flag & USE_LONG_NAMES ? name : lcp);

  *iid = icp;

  return SUCCESS;
}

/* Convert a tag (string) to an OID array              */
/* Tag can be either a symbolic name, or an OID string */
static struct tree *__tag2oid(char *tag, char *iid, oid *oid_arr,
                              int *oid_arr_len, int *type, int best_guess) {
  struct tree *tp = NULL;
  struct tree *rtp = NULL;
  oid newname[MAX_OID_LEN], *op;
  size_t newname_len = 0;

  if (type) {
    *type = TYPE_UNKNOWN;
  }
  if (oid_arr_len) {
    *oid_arr_len = 0;
  }
  if (!tag) {
    goto done;
  }

  /*********************************************************/
  /* best_guess = 0 - same as no switches (read_objid)     */
  /*                  if multiple parts, or uses find_node */
  /*                  if a single leaf                     */
  /* best_guess = 1 - same as -Ib (get_wild_node)          */
  /* best_guess = 2 - same as -IR (get_node)               */
  /*********************************************************/

  /* numeric scalar                (1,2) */
  /* single symbolic               (1,2) */
  /* single regex                  (1)   */
  /* partial full symbolic         (2)   */
  /* full symbolic                 (2)   */
  /* module::single symbolic       (2)   */
  /* module::partial full symbolic (2)   */
  if (best_guess == 1 || best_guess == 2) {
    /* make sure it's not a numeric tag */
    if (!__scan_num_objid(tag, newname, &newname_len)) {
      newname_len = MAX_OID_LEN;
      if (best_guess == 2) /* Random search -IR */
      {
        if (get_node(tag, newname, &newname_len)) {
          rtp = tp = get_tree(newname, newname_len, get_tree_head());
        }
      } else if (best_guess == 1) /* Regex search -Ib */
      {
        clear_tree_flags(get_tree_head());
        if (get_wild_node(tag, newname, &newname_len)) {
          rtp = tp = get_tree(newname, newname_len, get_tree_head());
        }
      }
    } else {
      rtp = tp = get_tree(newname, newname_len, get_tree_head());
    }
    if (type) {
      *type = (tp ? tp->type : TYPE_UNKNOWN);
    }
    if ((oid_arr == NULL) || (oid_arr_len == NULL)) {
      return rtp;
    }
    memcpy(oid_arr, (char *)newname, newname_len * sizeof(oid));
    *oid_arr_len = newname_len;
  }
  /* if best_guess is off and multi part tag or module::tag */
  /* numeric scalar                                         */
  /* module::single symbolic                                */
  /* module::partial full symbolic                          */
  /* FULL symbolic OID                                      */
  else if (strchr(tag, '.') || strchr(tag, ':')) {
    /* make sure it's not a numeric tag */
    if (!__scan_num_objid(tag, newname, &newname_len)) {
      newname_len = MAX_OID_LEN;
      if (read_objid(tag, newname, &newname_len)) /* long name */
      {
        rtp = tp = get_tree(newname, newname_len, get_tree_head());
      } else {
        /* failed to parse the OID */
        newname_len = 0;
      }
    } else {
      rtp = tp = get_tree(newname, newname_len, get_tree_head());
    }
    if (type) {
      *type = (tp ? tp->type : TYPE_UNKNOWN);
    }
    if ((oid_arr == NULL) || (oid_arr_len == NULL)) {
      return rtp;
    }
    memcpy(oid_arr, (char *)newname, newname_len * sizeof(oid));
    *oid_arr_len = newname_len;
  }
  /* else best_guess is off and it is a single leaf */
  /* single symbolic                                */
  else {
    rtp = tp = find_node(tag, get_tree_head());
    if (tp) {
      if (type) {
        *type = tp->type;
      }
      if ((oid_arr == NULL) || (oid_arr_len == NULL)) {
        return rtp;
      }
      /* code taken from get_node in snmp_client.c */
      for (op = newname + MAX_OID_LEN - 1; op >= newname; op--) {
        *op = tp->subid;
        tp = tp->parent;
        if (tp == NULL) {
          break;
        }
      }
      *oid_arr_len = newname + MAX_OID_LEN - op;
      memcpy(oid_arr, op, *oid_arr_len * sizeof(oid));
    } else {
      return rtp; /* HACK: otherwise, concat_oid_str confuses things */
    }
  }

done:

  if (iid && *iid && oid_arr_len) {
    __concat_oid_str(oid_arr, oid_arr_len, iid);
  }
  return rtp;
}

/* function: __concat_oid_str
 *
 * This function converts a dotted-decimal string, soid_str, to an array
 * of oid types and concatenates them on doid_arr begining at the index
 * specified by doid_arr_len.
 *
 * returns : SUCCESS, FAILURE
 */
static int __concat_oid_str(oid *doid_arr, int *doid_arr_len, char *soid_str) {
  char *soid_buf;
  char *cp;
  char *st;

  if (!soid_str || !*soid_str) {
    return SUCCESS; /* successfully added nothing */
  }
  if (*soid_str == '.') {
    soid_str++;
  }
  soid_buf = strdup(soid_str);
  if (!soid_buf) {
    return FAILURE;
  }
  cp = strtok_r(soid_buf, ".", &st);
  while (cp) {
    sscanf(cp, "%lu", &(doid_arr[(*doid_arr_len)++]));
    /* doid_arr[(*doid_arr_len)++] = atoi(cp); */
    cp = strtok_r(NULL, ".", &st);
  }
  free(soid_buf);
  return SUCCESS;
}

/* add a varbind to PDU */
static int __add_var_val_str(netsnmp_pdu *pdu, oid *name, int name_length,
                             char *val, int len, int type) {
  netsnmp_variable_list *vars;
  oid oidbuf[MAX_OID_LEN];
  int ret = SUCCESS;

  if (pdu->variables == NULL) {
    vars = (netsnmp_variable_list *)calloc(1, sizeof(netsnmp_variable_list));
    pdu->variables = vars;
  } else {
    /* make a copy of PDU variables */
    for (vars = pdu->variables; vars->next_variable;
         vars = vars->next_variable) {
      /* EXIT */;
    }

    vars->next_variable =
        (netsnmp_variable_list *)calloc(1, sizeof(netsnmp_variable_list));
    vars = vars->next_variable;
  }

  vars->next_variable = NULL;
  vars->name = snmp_duplicate_objid(name, name_length);
  vars->name_length = name_length;
  switch (type) {
  case TYPE_INTEGER:
  case TYPE_INTEGER32:
    vars->type = ASN_INTEGER;
    vars->val.integer = malloc(sizeof(long));
    if (val) {
      *(vars->val.integer) = strtol(val, NULL, 0);
    } else {
      ret = FAILURE;
      *(vars->val.integer) = 0;
    }
    vars->val_len = sizeof(long);
    break;

  case TYPE_GAUGE:
  case TYPE_UNSIGNED32:
    vars->type = ASN_GAUGE;
    goto UINT;
  case TYPE_COUNTER:
    vars->type = ASN_COUNTER;
    goto UINT;
  case TYPE_TIMETICKS:
    vars->type = ASN_TIMETICKS;
    goto UINT;
  case TYPE_UINTEGER:
    vars->type = ASN_UINTEGER;

  UINT:

    vars->val.integer = malloc(sizeof(long));
    if (val) {
      sscanf(val, "%lu", vars->val.integer);
    } else {
      ret = FAILURE;
      *(vars->val.integer) = 0;
    }
    vars->val_len = sizeof(long);
    break;

  case TYPE_OCTETSTR:
    vars->type = ASN_OCTET_STR;
    goto OCT;

  case TYPE_BITSTRING:
    vars->type = ASN_OCTET_STR;
    goto OCT;

  case TYPE_OPAQUE:
    vars->type = ASN_OCTET_STR;

  OCT:

    vars->val.string = malloc(len);
    vars->val_len = len;
    if (val && len) {
      memcpy((char *)vars->val.string, val, len);
    } else {
      ret = FAILURE;
      vars->val.string = (u_char *)strdup("");
      vars->val_len = 0;
    }
    break;

  case TYPE_IPADDR:
    vars->type = ASN_IPADDRESS;
    {
      in_addr_t addr;

      if (val) {
        addr = inet_addr(val);
      } else {
        ret = FAILURE;
        addr = 0;
      }
      vars->val.integer = compat_netsnmp_memdup(&addr, sizeof(addr));
      vars->val_len = sizeof(addr);
    }
    break;

  case TYPE_OBJID:
    vars->type = ASN_OBJECT_ID;
    vars->val_len = MAX_OID_LEN;
    /* if (read_objid(val, oidbuf, &(vars->val_len))) { */
    /* tp = __tag2oid(val, NULL, oidbuf, &(vars->val_len), NULL, 0); */
    if (!val || !snmp_parse_oid(val, oidbuf, &vars->val_len)) {
      vars->val.objid = NULL;
      ret = FAILURE;
    } else {
      vars->val.objid = snmp_duplicate_objid(oidbuf, vars->val_len);
      vars->val_len *= sizeof(oid);
    }
    break;

  default:
    vars->type = ASN_NULL;
    vars->val_len = 0;
    vars->val.string = NULL;
    ret = FAILURE;
  }

  return ret;
}

/* takes ss and pdu as input and updates the 'response' argument */
/* the input 'pdu' argument will be freed */
static int __send_sync_pdu(netsnmp_session *ss, netsnmp_pdu *pdu,
                           netsnmp_pdu **response, int retry_nosuch,
                           char *err_str, int *err_num, int *err_ind,
                           bitarray *invalid_oids) {
  int status = 0;
  long command = pdu->command;
  char *tmp_err_str;
  size_t retry_num = 0;

  /* Note: SNMP uses 1-based indexing with OIDs, so 0 is unused */
  unsigned long last_errindex = 0;

  *err_num = 0;
  *err_ind = 0;
  *response = NULL;
  tmp_err_str = NULL;
  memset(err_str, '\0', STR_BUF_SIZE);

  if (ss == NULL) {
    *err_num = 0;
    *err_ind = SNMPERR_BAD_SESSION;
    status = SNMPERR_BAD_SESSION;
    strlcpy(err_str, snmp_api_errstring(*err_ind), STR_BUF_SIZE);
    goto done;
  }

retry:

  Py_BEGIN_ALLOW_THREADS
  status = snmp_sess_synch_response(ss, pdu, response);
  Py_END_ALLOW_THREADS

      if ((*response == NULL) && (status == STAT_SUCCESS)) {
    status = STAT_ERROR;
  }

  // SNMP v3 doesn't quite raise timeouts correctly, so we correct it
  if (strcmp(err_str, "Timeout") && (status == STAT_ERROR)) {
    status = STAT_TIMEOUT;
  }

  switch (status) {
  case STAT_SUCCESS:
    status = (*response)->errstat;
    switch (status) {
    case SNMP_ERR_NOERROR:
      break;

    case SNMP_ERR_NOSUCHNAME:

      /*
      * if retry_nosuch is set, then remove the offending
      * OID which returns with NoSuchName, until none exist.
      */
      if (retry_nosuch) {
        /*
        * When using retry, we expect the agent to behave
        * in two ways:
        *
        *  (1) provide error index in descending order (easy case)
        *  (2) provide error index in ascending order (hard case)
        *
        *  The reason (2) is hard, is because everytime an OID
        *  is elided in the request PDU, we need to compensate.
        *
        *  It is possible that the agent may perform pathologically
        *  in which case we provide no guarantees whatsoever.
        */

        if (!last_errindex) {
          /* we haven't seen an errindex yet */
          bitarray_set_bit(invalid_oids, (*response)->errindex - 1);
        } else if (last_errindex > (*response)->errindex) {
          /* case (1) where error index is in descending order */
          bitarray_set_bit(invalid_oids, (*response)->errindex - 1);
        } else {
          /* case (2) where error index is in ascending order */
          bitarray_set_bit(invalid_oids, (*response)->errindex - 1 + retry_num);
        }

        /* finally we update the last_errindex for the next retry */
        last_errindex = (*response)->errindex;

        /*
        * fix the GET REQUEST message using snmp_fix_pdu
        * which elidse variable which return NOSUCHNAME error,
        * until there is either a successful response
        * (which indicates SNMP_ERR_NOERROR) or returns NULL
        * likely indicating no more remaining variables.
        */
        pdu = snmp_fix_pdu(*response, command);

        /*
        * The condition when pdu==NULL will happen when
        * there are no OIDs left to retry.
        */
        if (!pdu) {
          status = STAT_SUCCESS;
          goto done;
        }

        if (*response) {
          snmp_free_pdu(*response);
        }

        retry_num++;
        goto retry;
      } else /* !retry_nosuch */
      {
        PyErr_SetString(EasySNMPNoSuchNameError,
                        "no such name error encountered");
      }

      break;

    /* Pv1, SNMPsec, Pv2p, v2c, v2u, v2*, and SNMPv3 PDUs */
    case SNMP_ERR_TOOBIG:
    case SNMP_ERR_BADVALUE:
    case SNMP_ERR_READONLY:
    case SNMP_ERR_GENERR:
    /* in SNMPv2p, SNMPv2c, SNMPv2u, SNMPv2*, and SNMPv3 PDUs */
    case SNMP_ERR_NOACCESS:
    case SNMP_ERR_WRONGTYPE:
    case SNMP_ERR_WRONGLENGTH:
    case SNMP_ERR_WRONGENCODING:
    case SNMP_ERR_WRONGVALUE:
    case SNMP_ERR_NOCREATION:
    case SNMP_ERR_INCONSISTENTVALUE:
    case SNMP_ERR_RESOURCEUNAVAILABLE:
    case SNMP_ERR_COMMITFAILED:
    case SNMP_ERR_UNDOFAILED:
    case SNMP_ERR_AUTHORIZATIONERROR:
    case SNMP_ERR_NOTWRITABLE:
    /* in SNMPv2c, SNMPv2u, SNMPv2*, and SNMPv3 PDUs */
    case SNMP_ERR_INCONSISTENTNAME:
    default:
      strlcpy(err_str, (char *)snmp_errstring((*response)->errstat),
              STR_BUF_SIZE);
      *err_num = (int)(*response)->errstat;
      *err_ind = (*response)->errindex;
      py_log_msg(ERROR, "sync PDU: %s", err_str);

      PyErr_SetString(EasySNMPError, err_str);
      break;
    }
    break;

  case STAT_TIMEOUT:
    snmp_sess_error(ss, err_num, err_ind, &tmp_err_str);
    strlcpy(err_str, tmp_err_str, STR_BUF_SIZE);
    py_log_msg(ERROR, "sync PDU: %s", err_str);

    PyErr_SetString(EasySNMPTimeoutError,
                    "timed out while connecting to remote host");
    break;

  case STAT_ERROR:
    snmp_sess_error(ss, err_num, err_ind, &tmp_err_str);
    strlcpy(err_str, tmp_err_str, STR_BUF_SIZE);
    py_log_msg(ERROR, "sync PDU: %s", err_str);

    PyErr_SetString(EasySNMPError, tmp_err_str);
    break;

  default:
    strcat(err_str, "send_sync_pdu: unknown status");
    *err_num = ss->s_snmp_errno;
    py_log_msg(ERROR, "sync PDU: %s", err_str);

    break;
  }

done:

  if (tmp_err_str) {
    free(tmp_err_str);
  }

  return status;
}

static PyObject *py_netsnmp_construct_varbind(void) {
  return PyObject_CallMethod(easysnmp_import, "SNMPVariable", NULL);
}

static int py_netsnmp_attr_string(PyObject *obj, char *attr_name, char **val,
                                  Py_ssize_t *len) {
  *val = NULL;
  if (obj && attr_name && PyObject_HasAttrString(obj, attr_name)) {
    PyObject *attr = PyObject_GetAttrString(obj, attr_name);
    if (attr) {
      int retval;

#if PY_MAJOR_VERSION >= 3
      // Encode the provided attribute using latin-1 into bytes and
      // retrieve its value and length
      PyObject *attr_bytes =
          PyUnicode_AsEncodedString(attr, "latin-1", "surrogateescape");
      if (!attr_bytes) {
        return -1;
      }
      retval = PyBytes_AsStringAndSize(attr_bytes, val, len);
#else
      retval = PyString_AsStringAndSize(attr, val, len);
#endif

      Py_DECREF(attr);
      return retval;
    }
  }

  return -1;
}

static long long py_netsnmp_attr_long(PyObject *obj, char *attr_name) {
  long long val = -1;

  if (obj && attr_name && PyObject_HasAttrString(obj, attr_name)) {
    PyObject *attr = PyObject_GetAttrString(obj, attr_name);
    if (attr) {
      val = PyLong_AsLong(attr);
      Py_DECREF(attr);
    }
  }

  return val;
}

static void *py_netsnmp_attr_void_ptr(PyObject *obj, char *attr_name) {
  void *val = NULL;

  if (obj && attr_name && PyObject_HasAttrString(obj, attr_name)) {
    PyObject *attr = PyObject_GetAttrString(obj, attr_name);
    if (attr) {
      val = PyLong_AsVoidPtr(attr);
      Py_DECREF(attr);
    }
  }

  return val;
}

static int py_netsnmp_attr_set_string(PyObject *obj, char *attr_name, char *val,
                                      size_t len) {
  int ret = -1;
  if (obj && attr_name) {
    PyObject *val_obj =
        PyUnicode_Decode(val, len, "latin-1", "surrogateescape");
    if (!val_obj) {
      return -1;
    }
    ret = PyObject_SetAttrString(obj, attr_name, val_obj);
    Py_DECREF(val_obj);
  }
  return ret;
}

/**
 * Update python session object error attributes.
 *
 * Copy the error info which may have been returned from __send_sync_pdu(...)
 * into the python object. This will allow the python code to determine if
 * an error occured during an snmp operation.
 *
 * Currently there are 3 attributes we care about
 *
 * error_number - Copy of the value of netsnmp_session.s_errno. This is the
 * system errno that was generated during our last call into the net-snmp
 * library.
 *
 * error_index - Copy of the value of netsmp_session.s_snmp_errno. These error
 * numbers are separate from the system errno's and describe SNMP errors.
 *
 * error_string - A string describing the error_index that was returned during
 * our last operation.
 *
 * @param[in] session The python object that represents our current Session
 * @param[in|out] err_str A string describing err_ind
 * @param[in|out] err_num The system errno currently stored by our session
 * @param[in|out] err_ind The snmp errno currently stored by our session
 */
static void __py_netsnmp_update_session_errors(PyObject *session, char *err_str,
                                               int err_num, int err_ind) {
  PyObject *tmp_for_conversion;

  py_netsnmp_attr_set_string(session, "error_string", err_str, STRLEN(err_str));

  tmp_for_conversion = PyLong_FromLong(err_num);
  if (!tmp_for_conversion) {
    return; /* nothing better to do? */
  }
  PyObject_SetAttrString(session, "error_number", tmp_for_conversion);
  Py_DECREF(tmp_for_conversion);

  tmp_for_conversion = PyLong_FromLong(err_ind);
  if (!tmp_for_conversion) {
    return; /* nothing better to do? */
  }
  PyObject_SetAttrString(session, "error_index", tmp_for_conversion);
  Py_DECREF(tmp_for_conversion);
}

static PyObject *create_session_capsule(SnmpSession *session) {
  void *handle = NULL;
  session_capsule_ctx *ctx = NULL;
  PyObject *capsule = NULL;

  /* create a long lived handle from throwaway session object */
  if (!(handle = snmp_sess_open(session))) {
    PyErr_SetString(EasySNMPConnectionError, "couldn't create SNMP handle");
    goto except;
  }

  if (!(ctx = malloc(sizeof *ctx))) {
    PyErr_SetString(PyExc_RuntimeError,
                    "could not malloc() session_capsule_ctx");
    goto except;
  }

  /*
   * Create a capsule containing the ctx pointer with an "anonymous" name,
   * which is automatically destroyed by delete_session_capsule() when
   * no more references to the object are held.
   */
  if (!(capsule = PyCapsule_New(ctx, NULL, delete_session_capsule))) {
    PyErr_SetString(PyExc_RuntimeError,
                    "failed to create Python Capsule object");
    goto except;
  }

  /* init session context variables */
  ctx->handle = handle;
  ctx->snmp_version = 0;
  ctx->getlabel_flag = NO_FLAGS;
  ctx->sprintval_flag = USE_BASIC;
  ctx->old_format = 0;
  ctx->best_guess = 0;
  ctx->retry_nosuch = 0;
  return (capsule);

except:

  if (handle) {
    snmp_sess_close(handle);
  }

  if (ctx) {
    free(ctx);
  }

  if (capsule) {
    Py_XDECREF(capsule);
  }

  return NULL;
}

static void *get_session_context(PyObject *session) {

  char *tmpstr = NULL;
  Py_ssize_t tmplen;
  session_capsule_ctx *ctx = NULL;
  py_log_msg(DEBUG, "Getting session ptr");
  PyObject *session_capsule = PyObject_GetAttrString(session, "sess_ptr");

  if (!session_capsule) {
    PyErr_SetString(PyExc_RuntimeError,
                    "NULL arg calling get_session_context_from_capsule");

    goto done;
  }

  py_log_msg(DEBUG, "getting session capsule");
  ctx = PyCapsule_GetPointer(session_capsule, NULL);

  if (ctx) {
    py_log_msg(DEBUG, "Got session capsule");
    ctx->getlabel_flag = NO_FLAGS;
    ctx->sprintval_flag = USE_BASIC;
    ctx->snmp_version = py_netsnmp_attr_long(session, "version");

    if (py_netsnmp_attr_string(session, "error_string", &tmpstr, &tmplen) <
        0) {
      goto done;
    }
    memcpy(&ctx->err_str, tmpstr, tmplen);
    ctx->err_num = py_netsnmp_attr_long(session, "error_number");
    ctx->err_ind = py_netsnmp_attr_long(session, "error_index");

    ctx->old_format = netsnmp_ds_get_int(NETSNMP_DS_LIBRARY_ID,
                                    NETSNMP_DS_LIB_OID_OUTPUT_FORMAT);

    if (py_netsnmp_attr_long(session, "use_long_names")) {
      /*
      ** Set up for numeric or full OID's, if necessary.  Save the old
      ** output format so that it can be restored when we finish -- this
      ** is a library-wide global, and has to be set/restored for each
      ** session.
      */
      ctx->getlabel_flag |= USE_LONG_NAMES;

      netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID,
                         NETSNMP_DS_LIB_OID_OUTPUT_FORMAT,
                         NETSNMP_OID_OUTPUT_FULL);
    } else if (py_netsnmp_attr_long(session, "use_numeric")) {
      /*
       * Setting use_numeric forces use_long_names on so check for
       * use_numeric after use_long_names (above) to make sure the final
       * outcome of NETSNMP_DS_LIB_OID_OUTPUT_FORMAT is
       * NETSNMP_OID_OUTPUT_NUMERIC
       */
      ctx->getlabel_flag |= USE_LONG_NAMES;
      ctx->getlabel_flag |= USE_NUMERIC_OIDS;

      netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID,
                         NETSNMP_DS_LIB_OID_OUTPUT_FORMAT,
                         NETSNMP_OID_OUTPUT_NUMERIC);
    }

    if (py_netsnmp_attr_long(session, "use_enums")) {
      ctx->sprintval_flag = USE_ENUMS;
    }

    if (py_netsnmp_attr_long(session, "use_sprint_value")) {
      ctx->sprintval_flag = USE_SPRINT_VALUE;
    }

    ctx->best_guess = py_netsnmp_attr_long(session, "best_guess");
    ctx->retry_nosuch = py_netsnmp_attr_long(session, "retry_no_such");
  }

done:
  Py_DECREF(session_capsule);
  return ctx;
}

static void delete_session_capsule(PyObject *session_capsule) {
  session_capsule_ctx *ctx = NULL;

  // Reset output format
  ctx = PyCapsule_GetPointer(session_capsule, NULL);
  netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_OID_OUTPUT_FORMAT,
                     ctx->old_format);

  if (ctx) {
    snmp_sess_close(ctx->handle);
    free(ctx);
  }
}

static void snmp_op_data_reset(snmp_op_data *data) {
  data->pdu = NULL;
  data->response = NULL;
  data->vars = NULL;
  data->oid_arr = NULL;
  data->oid_arr_len = NULL;
  data->initial_oid_str_arr = NULL;
  data->oid_str_arr = NULL;
  data->oid_idx_str_arr = NULL;
  data->initial_oid = NULL;
  data->varlist = NULL;
  data->varbind = NULL;
  data->varlist_len = 0;
  data->len = 0;
  data->type = 0;
  data->error = 0;
}

static int snmp_op_data_load(snmp_op_data *data, int best_guess) {
  int error = 0;
  int varlist_len = data->varlist_len = PySequence_Length(data->varlist);
  int varlist_ind = 0;
  PyObject *varlist_iter = PyObject_GetIter(data->varlist);
  PyObject *varbind = NULL;

  data->pdu = NULL;
  data->response = NULL;
  data->vars = NULL;
  data->len = 0;
  data->type = 0;

  data->initial_oid_str_arr = PyMem_New(char *, varlist_len);
  data->oid_str_arr = PyMem_New(char *, varlist_len);
  data->oid_idx_str_arr = PyMem_New(char *, varlist_len);
  data->oid_arr = PyMem_New(oid *, varlist_len);
  data->oid_arr_len = PyMem_New(int, varlist_len);

  for (varlist_ind = 0; varlist_ind < varlist_len; varlist_ind++) {
    data->oid_arr[varlist_ind] = PyMem_New(oid, MAX_OID_LEN);
    data->oid_arr_len[varlist_ind] = MAX_OID_LEN;
  }

  py_log_msg(DEBUG, "%s: Reading oids from varlist", data->op_name);
  varlist_ind = 0;
  while ((varbind = PyIter_Next(varlist_iter))) {
    if (py_netsnmp_attr_string(varbind, "oid", &data->oid_str_arr[varlist_ind],
                               NULL) >= 0 &&
        py_netsnmp_attr_string(varbind, "oid_index",
                               &data->oid_idx_str_arr[varlist_ind], NULL) >= 0) {

      data->initial_oid_str_arr[varlist_ind] = data->oid_str_arr[varlist_ind];

      py_log_msg(DEBUG, "%s: Initial oid(%s) oid_idx(%s)",
                 data->op_name,
                 data->oid_str_arr[varlist_ind], data->oid_idx_str_arr[varlist_ind]);

      __tag2oid(data->oid_str_arr[varlist_ind], data->oid_idx_str_arr[varlist_ind],
                data->oid_arr[varlist_ind], &data->oid_arr_len[varlist_ind],
                NULL, best_guess);
    } else {
      data->oid_arr_len[varlist_ind] = 0;
    }

    if (!data->oid_arr_len[varlist_ind]) {
      py_log_msg(ERROR, "%s: object id %s has length: %d", data->op_name,
        data->oid_str_arr[varlist_ind], data->oid_arr_len[varlist_ind]);

      PyErr_Format(
          EasySNMPUnknownObjectIDError, "unknown object id (%s)",
          (data->oid_str_arr[varlist_ind] ? data->oid_str_arr[varlist_ind] : "<null>"));
      error = 1;
      Py_XDECREF(varbind);
      goto done;
    }

    Py_DECREF(varbind);
    varlist_ind++;
  }

done:
  Py_XDECREF(varlist_iter);
  return error;
}

static void snmp_op_data_finish(snmp_op_data *data) {
  if (data->pdu) {
    snmp_free_pdu(data->pdu);
  }

  if (data->response) {
    snmp_free_pdu(data->response);
  }

  SAFE_FREE(data->oid_arr_len);
  int varlist_len = data->varlist_len;
  int varlist_ind = 0;
  while (varlist_ind < varlist_len) {
    SAFE_FREE(data->oid_arr[varlist_ind]);
    varlist_ind++;
  }
  SAFE_FREE(data->oid_arr);
  SAFE_FREE(data->oid_str_arr);
  SAFE_FREE(data->oid_idx_str_arr);
  SAFE_FREE(data->initial_oid_str_arr);

  snmp_op_data_reset(data);
}

static int send_pdu_request(session_capsule_ctx *session_ctx, snmp_op_data* data) {
  int status = __send_sync_pdu(session_ctx->handle, data->pdu, &data->response, session_ctx->retry_nosuch, session_ctx->err_str,
                           &session_ctx->err_num, &session_ctx->err_ind, NULL);

  data->pdu = NULL;

  return status;
}

static PyObject *read_variable(netsnmp_variable_list *vars, snmp_op_data* data, int getlabel_flag, int sprintval_flag) {
  PyObject *varbind = py_netsnmp_construct_varbind();
  struct tree *tp = NULL;
  char *op_name = data->op_name;
  char *oid = NULL;
  char *oid_idx = NULL;
  int val_type = 0;
  char val_type_str[MAX_TYPE_NAME_LEN];
  int val_len = 0;

  u_char *str_buf = data->str_buf;
  size_t str_buf_len = sizeof(data->str_buf);
  size_t out_len = 0;
  int buf_over = 0;

  *data->str_buf = '.';
  *(data->str_buf + 1) = '\0';
  py_log_msg(DEBUG, "%s: str_buf: %s:%lu:%lu", op_name, str_buf,
             str_buf_len, out_len);
  tp = netsnmp_sprint_realloc_objid_tree(&str_buf, &str_buf_len,
                                         &out_len, 0, &buf_over,
                                         vars->name, vars->name_length);

  data->str_buf[sizeof(data->str_buf) - 1] = '\0';
  py_log_msg(DEBUG, "%s: str_buf: %s:%lu:%lu", op_name, str_buf,
             str_buf_len, out_len);

  val_type = __translate_asn_type(vars->type);

  if (__is_leaf(tp)) {
    py_log_msg(DEBUG, "%s: is_leaf: %d", op_name, tp->type);
  } else {
    getlabel_flag |= NON_LEAF_NAME;
    py_log_msg(DEBUG, "%s: !is_leaf: %d", op_name, tp->type);
  }

  py_log_msg(DEBUG, "%s: str_buf: %s", op_name, str_buf);

  // Set varbind properties
  py_netsnmp_attr_set_string(varbind, "root_oid", data->initial_oid, STRLEN(data->initial_oid));

  __get_label_iid((char *)data->str_buf, &oid, &oid_idx, getlabel_flag);
  py_netsnmp_attr_set_string(varbind, "oid", oid, STRLEN(oid));
  py_netsnmp_attr_set_string(varbind, "oid_index", oid_idx, STRLEN(oid_idx));

  __get_type_str(val_type, val_type_str, 1);
  py_netsnmp_attr_set_string(varbind, "snmp_type", val_type_str,
                             strlen(val_type_str));

  val_len = __snprint_value((char *)data->str_buf, sizeof(data->str_buf), vars, tp, val_type,
                        sprintval_flag);
  str_buf[val_len] = '\0';
  py_netsnmp_attr_set_string(varbind, "value", (char *)str_buf, val_len);

  return varbind;
}

static PyObject *netsnmp_create_session(PyObject *self, PyObject *args) {
  int version;
  char *community;
  char *peer;
  int lport;
  int retries;
  int timeout;
  SnmpSession session = {0};

  if (!PyArg_ParseTuple(args, "issiii", &version, &community, &peer, &lport,
                        &retries, &timeout)) {
    goto done;
  }

  snmp_sess_init(&session);

  session.version = -1;
#ifndef DISABLE_SNMPV1
  if (version == 1) {
    session.version = SNMP_VERSION_1;
  }
#endif
#ifndef DISABLE_SNMPV2C
  if (version == 2) {
    session.version = SNMP_VERSION_2c;
  }
#endif
  if (version == 3) {
    session.version = SNMP_VERSION_3;
  }
  if (session.version == -1) {
    PyErr_Format(PyExc_ValueError, "unsupported SNMP version (%d)", version);
    goto done;
  }

  session.community_len = STRLEN((char *)community);
  session.community = (u_char *)community;
  session.peername = peer;
  session.local_port = lport;
  session.retries = retries; /* 5 */
  session.timeout = timeout; /* 1000000L */
  session.authenticator = NULL;

  return create_session_capsule(&session);

done:

  return NULL;
}

static PyObject *netsnmp_create_session_v3(PyObject *self, PyObject *args) {
  int version;
  char *peer;
  int lport;
  int retries;
  int timeout;
  char *sec_name;
  int sec_level;
  char *sec_eng_id;
  char *context_eng_id;
  char *context;
  char *auth_proto;
  char *auth_pass;
  char *priv_proto;
  char *priv_pass;
  int eng_boots;
  int eng_time;
  SnmpSession session = {0};
  int md5_enabled = 1;
  int des_enabled = 1;

#ifndef DISABLE_MD5
  md5_enabled = 0;
#endif
#ifndef DISABLE_DES
  des_enabled = 0;
#endif

  if (!PyArg_ParseTuple(args, "isiiisisssssssii", &version, &peer, &lport,
                        &retries, &timeout, &sec_name, &sec_level, &sec_eng_id,
                        &context_eng_id, &context, &auth_proto, &auth_pass,
                        &priv_proto, &priv_pass, &eng_boots, &eng_time)) {
    return NULL;
  }

  snmp_sess_init(&session);

  if (version == 3) {
    session.version = SNMP_VERSION_3;
  } else {
    PyErr_Format(PyExc_ValueError, "unsupported SNMP version (%d)", version);
    goto done;
  }

  session.peername = peer;
  session.retries = retries; /* 5 */
  session.timeout = timeout; /* 1000000L */
  session.authenticator = NULL;
  session.contextNameLen = STRLEN(context);
  session.contextName = context;
  session.securityNameLen = STRLEN(sec_name);
  session.securityName = sec_name;
  session.securityLevel = sec_level;
  session.securityModel = USM_SEC_MODEL_NUMBER;
  session.securityEngineIDLen =
      hex_to_binary2((unsigned char *)sec_eng_id, STRLEN(sec_eng_id),
                     (char **)&session.securityEngineID);
  session.contextEngineIDLen =
      hex_to_binary2((unsigned char *)context_eng_id, STRLEN(sec_eng_id),
                     (char **)&session.contextEngineID);
  session.engineBoots = eng_boots;
  session.engineTime = eng_time;


    if ((md5_enabled == 1) && !strcmp(auth_proto, "MD5")) {
      session.securityAuthProto =
          snmp_duplicate_objid(usmHMACMD5AuthProtocol, USM_AUTH_PROTO_MD5_LEN);
      session.securityAuthProtoLen = USM_AUTH_PROTO_MD5_LEN;
    } else if (!strcmp(auth_proto, "SHA")) {
    session.securityAuthProto =
        snmp_duplicate_objid(usmHMACSHA1AuthProtocol, USM_AUTH_PROTO_SHA_LEN);
    session.securityAuthProtoLen = USM_AUTH_PROTO_SHA_LEN;
  } else if (!strcmp(auth_proto, "DEFAULT")) {
    const oid *a = get_default_authtype(&session.securityAuthProtoLen);
    session.securityAuthProto =
        snmp_duplicate_objid(a, session.securityAuthProtoLen);
  } else {
    PyErr_Format(PyExc_ValueError, "unsupported authentication protocol (%s)",
                 auth_proto);
    goto done;
  }
  if (session.securityLevel >= SNMP_SEC_LEVEL_AUTHNOPRIV) {
    if (STRLEN(auth_pass) > 0) {
      session.securityAuthKeyLen = USM_AUTH_KU_LEN;
      if (generate_Ku(session.securityAuthProto, session.securityAuthProtoLen,
                      (u_char *)auth_pass, STRLEN(auth_pass),
                      session.securityAuthKey,
                      &session.securityAuthKeyLen) != SNMPERR_SUCCESS) {
        PyErr_SetString(EasySNMPConnectionError,
                        "error generating Ku from authentication "
                        "password");
        goto done;
      }
    }
  }
  if ((des_enabled == 1) && !strcmp(priv_proto, "DES")) {
    session.securityPrivProto =
        snmp_duplicate_objid(usmDESPrivProtocol, USM_PRIV_PROTO_DES_LEN);
    session.securityPrivProtoLen = USM_PRIV_PROTO_DES_LEN;
  } else if (!strncmp(priv_proto, "AES", 3)) {
    session.securityPrivProto =
        snmp_duplicate_objid(usmAESPrivProtocol, USM_PRIV_PROTO_AES_LEN);
    session.securityPrivProtoLen = USM_PRIV_PROTO_AES_LEN;
  } else if (!strcmp(priv_proto, "DEFAULT")) {
    const oid *p = get_default_privtype(&session.securityPrivProtoLen);
    session.securityPrivProto =
        snmp_duplicate_objid(p, session.securityPrivProtoLen);
  } else {
    PyErr_Format(PyExc_ValueError, "unsupported privacy protocol (%s)",
                 priv_proto);
    goto done;
  }

  if (session.securityLevel >= SNMP_SEC_LEVEL_AUTHPRIV) {
    session.securityPrivKeyLen = USM_PRIV_KU_LEN;
    if (generate_Ku(session.securityAuthProto, session.securityAuthProtoLen,
                    (u_char *)priv_pass, STRLEN(priv_pass),
                    session.securityPrivKey,
                    &session.securityPrivKeyLen) != SNMPERR_SUCCESS) {
      PyErr_SetString(EasySNMPConnectionError,
                      "couldn't gen Ku from priv pass phrase");
      goto done;
    }
  }

  return create_session_capsule(&session);

done:

  SAFE_FREE(session.securityEngineID);
  SAFE_FREE(session.contextEngineID);

  return NULL;
}

static PyObject *netsnmp_create_session_tunneled(PyObject *self,
                                                 PyObject *args) {
  int version;
  char *peer;
  int lport;
  int retries;
  int timeout;
  char *sec_name;
  int sec_level;
  char *context_eng_id;
  char *context;
  char *our_identity;
  char *their_identity;
  char *their_hostname;
  char *trust_cert;
  SnmpSession session = {0};

  if (!PyArg_ParseTuple(args, "isiiisissssss", &version, &peer, &lport,
                        &retries, &timeout, &sec_name, &sec_level,
                        &context_eng_id, &context, &our_identity,
                        &their_identity, &their_hostname, &trust_cert)) {
    return NULL;
  }

  if (version != 3) {
    PyErr_SetString(PyExc_ValueError,
                    "you must use SNMP version 3 as it's the only "
                    "version that supports tunneling");
    return NULL;
  }

  snmp_sess_init(&session);

  session.peername = peer;
  session.retries = retries; /* 5 */
  session.timeout = timeout; /* 1000000L */
  session.contextNameLen = STRLEN(context);
  session.contextName = context;
  session.securityNameLen = STRLEN(sec_name);
  session.securityName = sec_name;
  session.securityLevel = sec_level;
  session.securityModel = NETSNMP_TSM_SECURITY_MODEL;

  /* create the transport configuration store */
  if (!session.transport_configuration) {
    netsnmp_container_init_list();
    session.transport_configuration =
        netsnmp_container_find("transport_configuration:fifo");
    if (!session.transport_configuration) {
      py_log_msg(ERROR, "failed to initialize the transport "
                        "configuration container");
      goto done;
    }

    session.transport_configuration->compare =
        (netsnmp_container_compare *)netsnmp_transport_config_compare;
  }

  if (our_identity && our_identity[0] != '\0')
    CONTAINER_INSERT(
        session.transport_configuration,
        netsnmp_transport_create_config("localCert", our_identity));

  if (their_identity && their_identity[0] != '\0')
    CONTAINER_INSERT(
        session.transport_configuration,
        netsnmp_transport_create_config("peerCert", their_identity));

  if (their_hostname && their_hostname[0] != '\0')
    CONTAINER_INSERT(
        session.transport_configuration,
        netsnmp_transport_create_config("their_hostname", their_hostname));

  if (trust_cert && trust_cert[0] != '\0')
    CONTAINER_INSERT(session.transport_configuration,
                     netsnmp_transport_create_config("trust_cert", trust_cert));

  return create_session_capsule(&session);

done:

  return NULL;
}

static PyObject *netsnmp_get(PyObject *self, PyObject *args) {
  PyObject *session = NULL;
  session_capsule_ctx *session_ctx = NULL;
  snmp_op_data op_data;
  PyObject *result_varlist = NULL;
  netsnmp_variable_list *vars = NULL;
  int error = 0;
  int op_data_error = 0;
  char *op_name = "netsnmp_get";

  snmp_op_data_reset(&op_data);

  py_log_msg(DEBUG, "%s: Starting", op_name);

  if (!args) {
    const char *err_msg = "%s: missing arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  if (!PyArg_ParseTuple(args, "OO", &session, &op_data.varlist)) {
    const char *err_msg = "%s: Could not parse arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Arguments parsed", op_name);

  if (!PyList_Check(op_data.varlist)) {
    const char *err_msg = "%s: varlist is not a list";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Getting session context", op_name);
  session_ctx = get_session_context(session);

  if (!session_ctx) {
    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Got session context", op_name);

  py_log_msg(DEBUG, "%s: Loading operation data", op_name);
  op_data.op_name = op_name;
  op_data_error = snmp_op_data_load(&op_data, session_ctx->best_guess);

  if (op_data_error || PyErr_Occurred()) {
    if(PyErr_Occurred()) {
      goto exception;
    }
    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Finished loading operation data", op_name);

  py_log_msg(DEBUG, "%s: Starting snmp request", op_name);

  result_varlist = PyList_New(0);
  PyObject * varbind = NULL;
  int notdone = 0;
  int status = 0;
  int varlist_ind = 0;
  int varlist_len = op_data.varlist_len;

  while (varlist_ind < varlist_len) {
    op_data.pdu = snmp_pdu_create(SNMP_MSG_GET);
    snmp_add_null_var(op_data.pdu, op_data.oid_arr[varlist_ind], op_data.oid_arr_len[varlist_ind]);

    py_log_msg(DEBUG, "%s: filling request: oid(%s) "
                      "oid_idx(%s) oid_arr_len(%d) best_guess(%d)",
                op_data.op_name,
                op_data.oid_str_arr[varlist_ind],
                op_data.oid_idx_str_arr[varlist_ind],
                op_data.oid_arr_len[varlist_ind],
                session_ctx->best_guess);

    notdone = 1;
    while (notdone) {
      py_log_msg(DEBUG, "%s: Sending pdu req",
        op_data.op_name);

      status = send_pdu_request(session_ctx, &op_data);
      if((status != STAT_SUCCESS) || PyErr_Occurred()) {

        if(PyErr_Occurred()) {
          goto exception;
        }

        py_log_msg(ERROR, "%s: PDU req resulted in error Request Status(%d)",
          op_data.op_name, status);
        error = 1;
        goto done;
      }

      if (notdone) {
        vars = op_data.response->variables;
        op_data.initial_oid = op_data.initial_oid_str_arr[varlist_ind];

        while (vars) {

          if (vars->type == SNMP_ENDOFMIBVIEW) {
            py_log_msg(DEBUG, "%s: encountered end condition "
                              "(ENDOFMIBVIEW)", op_name);
            notdone = 0;
            break;
          }
          else if ((vars->type == SNMP_NOSUCHOBJECT) ||
          (vars->type == SNMP_NOSUCHINSTANCE)) {
            char val_type_str[MAX_TYPE_NAME_LEN];

            __get_type_str(vars->type, val_type_str, 1);
            varbind = py_netsnmp_construct_varbind();

            py_netsnmp_attr_set_string(varbind, "root_oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "snmp_type", val_type_str,
                                       strlen(val_type_str));

            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);

            notdone = 0;
            break;
          }
          else if ((vars->name_length < op_data.oid_arr_len[varlist_ind]) ||
              (memcmp(op_data.oid_arr[varlist_ind], vars->name,
                      op_data.oid_arr_len[varlist_ind] * sizeof(oid)) != 0)) {
            py_log_msg(DEBUG, "%s: encountered end condition (next subtree iteration out of scope) var_len: %d/op_var_len: %d var_name_size: %d/op_name_size: %d",
              op_name, vars->name_length, op_data.oid_arr_len[varlist_ind],
              sizeof(op_data.oid_arr[varlist_ind]),
              sizeof(vars->name)
            );
            notdone = 0;
            break;
          }

          varbind = read_variable(vars, &op_data, session_ctx->getlabel_flag, session_ctx->sprintval_flag);

          if (varbind) {
            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);
          } else {
            py_log_msg(ERROR, "%s bad varbind (%d)", op_name, varlist_ind);
          }

          vars = vars->next_variable;
        }

        py_log_msg(DEBUG,
          "%s: Finished reading all variables from request",
          op_name
        );
        notdone = 0;
      }

      if (op_data.response) {
        snmp_free_pdu(op_data.response);
        op_data.response = NULL;
      }
    }
    varlist_ind++;
  }
  goto done;

exception:
  snmp_op_data_finish(&op_data);
  return NULL;

done:
  py_log_msg(DEBUG, "%s: Starting cleanup", op_name);

  snmp_op_data_finish(&op_data);

  if(error) {
    py_log_msg(ERROR, "%s: Exiting due to error %d", op_name, error);

    __py_netsnmp_update_session_errors(session, session_ctx->err_str,
      session_ctx->err_num, session_ctx->err_ind);

    Py_XDECREF(result_varlist);
    return NULL;
  }

  py_log_msg(DEBUG, "%s: End cleanup", op_name);

  py_log_msg(DEBUG, "%s: Returning %d objects", op_name, PySequence_Length(result_varlist));

  return Py_BuildValue("N", result_varlist);
}

static PyObject *netsnmp_getnext(PyObject *self, PyObject *args) {
  PyObject *session = NULL;
  session_capsule_ctx *session_ctx = NULL;
  snmp_op_data op_data;
  PyObject *result_varlist = NULL;
  netsnmp_variable_list *vars = NULL;
  int error = 0;
  int op_data_error = 0;
  char *op_name = "netsnmp_getnext";
  snmp_op_data_reset(&op_data);

  py_log_msg(DEBUG, "%s: Starting", op_name);

  if (!args) {
    const char *err_msg = "%s: missing arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  if (!PyArg_ParseTuple(args, "OO", &session, &op_data.varlist)) {
    const char *err_msg = "%s: Could not parse arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Arguments parsed", op_name);

  if (!PyList_Check(op_data.varlist)) {
    const char *err_msg = "%s: varlist is not a list";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Getting session context", op_name);
  session_ctx = get_session_context(session);

  if (!session_ctx) {
    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Got session context", op_name);

  py_log_msg(DEBUG, "%s: Loading operation data", op_name);
  op_data.op_name = op_name;
  op_data_error = snmp_op_data_load(&op_data, session_ctx->best_guess);

  if (op_data_error || PyErr_Occurred()) {
    if(PyErr_Occurred()) {
      goto exception;
    }

    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Finished loading operation data", op_name);

  py_log_msg(DEBUG, "%s: Starting snmp request", op_name);

  result_varlist = PyList_New(0);
  PyObject * varbind = NULL;
  int notdone = 0;
  int status = 0;
  int varlist_ind = 0;
  int varlist_len = op_data.varlist_len;

  while (varlist_ind < varlist_len) {
    op_data.pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
    snmp_add_null_var(op_data.pdu, op_data.oid_arr[varlist_ind], op_data.oid_arr_len[varlist_ind]);

    py_log_msg(DEBUG, "%s: filling request: oid(%s) "
                      "oid_idx(%s) oid_arr_len(%d) best_guess(%d)",
                op_data.op_name,
                op_data.oid_str_arr[varlist_ind],
                op_data.oid_idx_str_arr[varlist_ind],
                op_data.oid_arr_len[varlist_ind],
                session_ctx->best_guess);

    notdone = 1;
    while (notdone) {
      py_log_msg(DEBUG, "%s: Sending pdu req",
        op_data.op_name);

      status = send_pdu_request(session_ctx, &op_data);
      if((status != STAT_SUCCESS) || PyErr_Occurred()) {

        if(PyErr_Occurred()) {
          goto exception;
        }

        py_log_msg(ERROR, "%s: PDU req resulted in error Request Status(%d)",
          op_data.op_name, status);
        error = 1;
        goto done;
      }

      if (notdone) {
        vars = op_data.response->variables;
        op_data.initial_oid = op_data.initial_oid_str_arr[varlist_ind];

        while (vars) {

          if (vars->type == SNMP_ENDOFMIBVIEW) {
            py_log_msg(DEBUG, "%s: encountered end condition "
                              "(ENDOFMIBVIEW)", op_name);
            notdone = 0;
            break;
          }
          else if ((vars->type == SNMP_NOSUCHOBJECT) ||
          (vars->type == SNMP_NOSUCHINSTANCE)) {
            char val_type_str[MAX_TYPE_NAME_LEN];

            __get_type_str(vars->type, val_type_str, 1);
            varbind = py_netsnmp_construct_varbind();

            py_netsnmp_attr_set_string(varbind, "root_oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "snmp_type", val_type_str,
                                       strlen(val_type_str));

            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);

            notdone = 0;
            break;
          }
          else if ((vars->name_length < op_data.oid_arr_len[varlist_ind]) ||
              (memcmp(op_data.oid_arr[varlist_ind], vars->name,
                      op_data.oid_arr_len[varlist_ind] * sizeof(oid)) != 0)) {
            py_log_msg(DEBUG, "%s: encountered end condition (next subtree iteration out of scope) var_len: %d/op_var_len: %d var_name_size: %d/op_name_size: %d",
              op_name, vars->name_length, op_data.oid_arr_len[varlist_ind],
              sizeof(op_data.oid_arr[varlist_ind]),
              sizeof(vars->name)
            );
            notdone = 0;
            break;
          }

          varbind = read_variable(vars, &op_data, session_ctx->getlabel_flag, session_ctx->sprintval_flag);

          if (varbind) {
            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);
          } else {
            py_log_msg(ERROR, "%s bad varbind (%d)", op_name, varlist_ind);
          }

          vars = vars->next_variable;
        }

        py_log_msg(DEBUG,
          "%s: Finished reading all variables from request",
          op_name
        );
        notdone = 0;
      }

      if (op_data.response) {
        snmp_free_pdu(op_data.response);
        op_data.response = NULL;
      }
    }
    varlist_ind++;
  }
  goto done;

exception:
  snmp_op_data_finish(&op_data);
  return NULL;

done:
  py_log_msg(DEBUG, "%s: Starting cleanup", op_name);

  snmp_op_data_finish(&op_data);

  if(error) {
    py_log_msg(ERROR, "%s: Exiting due to error %d", op_name, error);

    __py_netsnmp_update_session_errors(session, session_ctx->err_str,
      session_ctx->err_num, session_ctx->err_ind);

    Py_XDECREF(result_varlist);
    return NULL;
  }

  py_log_msg(DEBUG, "%s: End cleanup", op_name);

  py_log_msg(DEBUG, "%s: Returning %d objects", op_name, PySequence_Length(result_varlist));

  return Py_BuildValue("N", result_varlist);
}

static PyObject *netsnmp_walk(PyObject *self, PyObject *args) {
  PyObject *session = NULL;
  session_capsule_ctx *session_ctx = NULL;
  snmp_op_data op_data;
  PyObject *result_varlist = NULL;
  netsnmp_variable_list *vars = NULL;
  int error = 0;
  int op_data_error = 0;
  char *op_name = "netsnmp_walk";
  snmp_op_data_reset(&op_data);

  py_log_msg(DEBUG, "%s: Starting", op_name);

  if (!args) {
    const char *err_msg = "%s: missing arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  if (!PyArg_ParseTuple(args, "OO", &session, &op_data.varlist)) {
    const char *err_msg = "%s: Could not parse arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Arguments parsed", op_name);

  if (!PyList_Check(op_data.varlist)) {
    const char *err_msg = "%s: varlist is not a list";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Getting session context", op_name);
  session_ctx = get_session_context(session);

  if (!session_ctx) {
    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Got session context", op_name);

  py_log_msg(DEBUG, "%s: Loading operation data", op_name);
  op_data.op_name = op_name;
  op_data_error = snmp_op_data_load(&op_data, session_ctx->best_guess);

  if (op_data_error || PyErr_Occurred()) {
    if(PyErr_Occurred()) {
      goto exception;
    }

    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Finished loading operation data", op_name);

  py_log_msg(DEBUG, "%s: Starting snmp request", op_name);

  result_varlist = PyList_New(0);
  PyObject * varbind = NULL;
  int notdone = 0;
  int status = 0;
  int varlist_ind = 0;
  int varlist_len = op_data.varlist_len;

  while (varlist_ind < varlist_len) {
    op_data.pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
    snmp_add_null_var(op_data.pdu, op_data.oid_arr[varlist_ind], op_data.oid_arr_len[varlist_ind]);

    py_log_msg(DEBUG, "%s: filling request: oid(%s) "
                      "oid_idx(%s) oid_arr_len(%d) best_guess(%d)",
                op_data.op_name,
                op_data.oid_str_arr[varlist_ind],
                op_data.oid_idx_str_arr[varlist_ind],
                op_data.oid_arr_len[varlist_ind],
                session_ctx->best_guess);

    notdone = 1;
    while (notdone) {
      py_log_msg(DEBUG, "%s: Sending pdu req",
        op_data.op_name);

      status = send_pdu_request(session_ctx, &op_data);
      if((status != STAT_SUCCESS) || PyErr_Occurred()) {

        if(PyErr_Occurred()) {
          goto exception;
        }

        py_log_msg(ERROR, "%s: PDU req resulted in error Request Status(%d)",
          op_data.op_name, status);
        error = 1;
        goto done;
      }

      if (notdone) {
        vars = op_data.response->variables;
        op_data.initial_oid = op_data.initial_oid_str_arr[varlist_ind];

        while (vars) {

          if (vars->type == SNMP_ENDOFMIBVIEW) {
            py_log_msg(DEBUG, "%s: encountered end condition "
                              "(ENDOFMIBVIEW)", op_name);
            notdone = 0;
            break;
          }
          else if ((vars->type == SNMP_NOSUCHOBJECT) ||
          (vars->type == SNMP_NOSUCHINSTANCE)) {
            char val_type_str[MAX_TYPE_NAME_LEN];

            __get_type_str(vars->type, val_type_str, 1);
            varbind = py_netsnmp_construct_varbind();

            py_netsnmp_attr_set_string(varbind, "root_oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "snmp_type", val_type_str,
                                       strlen(val_type_str));

            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);

            notdone = 0;
            break;
          }
          else if ((vars->name_length < op_data.oid_arr_len[varlist_ind]) ||
              (memcmp(op_data.oid_arr[varlist_ind], vars->name,
                      op_data.oid_arr_len[varlist_ind] * sizeof(oid)) != 0)) {
            py_log_msg(DEBUG, "%s: encountered end condition (next subtree iteration out of scope) var_len: %d/op_var_len: %d var_name_size: %d/op_name_size: %d",
              op_name, vars->name_length, op_data.oid_arr_len[varlist_ind],
              sizeof(op_data.oid_arr[varlist_ind]),
              sizeof(vars->name)
            );
            notdone = 0;
            break;
          }

          varbind = read_variable(vars, &op_data, session_ctx->getlabel_flag, session_ctx->sprintval_flag);

          if (varbind) {
            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);
          } else {
            py_log_msg(ERROR, "%s bad varbind (%d)", op_name, varlist_ind);
          }

          // Create next request if we've reached the end
          if (vars->next_variable == NULL) {
            op_data.pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
            snmp_add_null_var(op_data.pdu, vars->name, vars->name_length);
            py_log_msg(DEBUG,
              "%s: Creating pdu request for %s",
              op_name, op_data.initial_oid
            );
          }

          // Move on to next
          vars = vars->next_variable;
        }

        py_log_msg(DEBUG,
          "%s: Partially finished reading all variables from request for %s",
          op_name, op_data.initial_oid
        );
      }

      if (op_data.response) {
        snmp_free_pdu(op_data.response);
        op_data.response = NULL;
        py_log_msg(DEBUG,
          "%s: Finished reading all variables from request for %s",
          op_name, op_data.initial_oid
        );
      }
    }
    varlist_ind++;
  }

  py_log_msg(DEBUG, "%s: Finished reading all variables",op_name);
  goto done;

exception:
  snmp_op_data_finish(&op_data);
  return NULL;

done:
  py_log_msg(DEBUG, "%s: Starting cleanup", op_name);

  snmp_op_data_finish(&op_data);

  if(error) {
    py_log_msg(ERROR, "%s: Exiting due to error %d", op_name, error);

    __py_netsnmp_update_session_errors(session, session_ctx->err_str,
      session_ctx->err_num, session_ctx->err_ind);

    Py_XDECREF(result_varlist);
    return NULL;
  }

  py_log_msg(DEBUG, "%s: End cleanup", op_name);

  py_log_msg(DEBUG, "%s: Returning %d objects", op_name, PySequence_Length(result_varlist));

  return Py_BuildValue("N", result_varlist);
}

static PyObject *netsnmp_getbulk(PyObject *self, PyObject *args) {
  PyObject *session = NULL;
  session_capsule_ctx *session_ctx = NULL;
  snmp_op_data op_data;
  PyObject *result_varlist = NULL;
  netsnmp_variable_list *vars = NULL;
  int error = 0;
  int op_data_error = 0;
  char *op_name = "netsnmp_getbulk";
  int nonrepeaters;
  int maxrepetitions;

  snmp_op_data_reset(&op_data);

  py_log_msg(DEBUG, "%s: Starting", op_name);

  if (!args) {
    const char *err_msg = "%s: missing arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  if (!PyArg_ParseTuple(args, "OOii", &session, &op_data.varlist, &nonrepeaters,
                        &maxrepetitions)) {
    const char *err_msg = "%s: Could not parse arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Arguments parsed", op_name);

  if (!PyList_Check(op_data.varlist)) {
    const char *err_msg = "%s: varlist is not a list";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Getting session context", op_name);
  session_ctx = get_session_context(session);

  if (!session_ctx) {
    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Got session context", op_name);

  py_log_msg(DEBUG, "%s: Loading operation data", op_name);
  op_data.op_name = op_name;
  op_data_error = snmp_op_data_load(&op_data, session_ctx->best_guess);

  if (op_data_error || PyErr_Occurred()) {
    if(PyErr_Occurred()) {
      goto exception;
    }

    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Finished loading operation data", op_name);

  py_log_msg(DEBUG, "%s: Starting snmp request", op_name);

  result_varlist = PyList_New(0);
  PyObject * varbind = NULL;
  int notdone = 0;
  int status = 0;
  int varlist_ind = 0;
  int varlist_len = op_data.varlist_len;

  while (varlist_ind < varlist_len) {
    op_data.pdu = snmp_pdu_create(SNMP_MSG_GETBULK);
    op_data.pdu->non_repeaters = nonrepeaters;
    op_data.pdu->max_repetitions = maxrepetitions;
    snmp_add_null_var(op_data.pdu, op_data.oid_arr[varlist_ind], op_data.oid_arr_len[varlist_ind]);

    py_log_msg(DEBUG, "%s: filling request: oid(%s) "
                      "oid_idx(%s) oid_arr_len(%d) best_guess(%d)",
                op_data.op_name,
                op_data.oid_str_arr[varlist_ind],
                op_data.oid_idx_str_arr[varlist_ind],
                op_data.oid_arr_len[varlist_ind],
                session_ctx->best_guess);

    notdone = 1;
    while (notdone) {
      py_log_msg(DEBUG, "%s: Sending pdu req",
        op_data.op_name);

      status = send_pdu_request(session_ctx, &op_data);
      if((status != STAT_SUCCESS) || PyErr_Occurred()) {

        if(PyErr_Occurred()) {
          goto exception;
        }

        py_log_msg(ERROR, "%s: PDU req resulted in error Request Status(%d)",
          op_data.op_name, status);
        error = 1;
        goto done;
      }

      if (notdone) {
        vars = op_data.response->variables;
        op_data.initial_oid = op_data.initial_oid_str_arr[varlist_ind];

        while (vars) {

          if (vars->type == SNMP_ENDOFMIBVIEW) {
            py_log_msg(DEBUG, "%s: encountered end condition "
                              "(ENDOFMIBVIEW)", op_name);
            notdone = 0;
            break;
          }
          else if ((vars->type == SNMP_NOSUCHOBJECT) ||
          (vars->type == SNMP_NOSUCHINSTANCE)) {
            char val_type_str[MAX_TYPE_NAME_LEN];

            __get_type_str(vars->type, val_type_str, 1);
            varbind = py_netsnmp_construct_varbind();

            py_netsnmp_attr_set_string(varbind, "root_oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "snmp_type", val_type_str,
                                       strlen(val_type_str));

            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);

            notdone = 0;
            break;
          }
          else if ((vars->name_length < op_data.oid_arr_len[varlist_ind]) ||
              (memcmp(op_data.oid_arr[varlist_ind], vars->name,
                      op_data.oid_arr_len[varlist_ind] * sizeof(oid)) != 0)) {
            py_log_msg(DEBUG, "%s: encountered end condition (next subtree iteration out of scope) var_len: %d/op_var_len: %d var_name_size: %d/op_name_size: %d",
              op_name, vars->name_length, op_data.oid_arr_len[varlist_ind],
              sizeof(op_data.oid_arr[varlist_ind]),
              sizeof(vars->name)
            );
            notdone = 0;
            break;
          }

          varbind = read_variable(vars, &op_data, session_ctx->getlabel_flag, session_ctx->sprintval_flag);

          if (varbind) {
            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);
          } else {
            py_log_msg(ERROR, "%s bad varbind (%d)", op_name, varlist_ind);
          }

          // Move on to next
          vars = vars->next_variable;
        }

        py_log_msg(DEBUG,
          "%s: Partially finished reading all variables from request for %s",
          op_name, op_data.initial_oid
        );
      }

      if (op_data.response) {
        snmp_free_pdu(op_data.response);
        op_data.response = NULL;
        py_log_msg(DEBUG,
          "%s: Finished reading all variables from request for %s",
          op_name, op_data.initial_oid
        );
        notdone = 0;
      }
    }
    varlist_ind++;
  }

  py_log_msg(DEBUG, "%s: Finished reading all variables",op_name);
  goto done;

exception:
  snmp_op_data_finish(&op_data);
  return NULL;

done:
  py_log_msg(DEBUG, "%s: Starting cleanup", op_name);

  snmp_op_data_finish(&op_data);

  if(error) {
    py_log_msg(ERROR, "%s: Exiting due to error %d", op_name, error);

    __py_netsnmp_update_session_errors(session, session_ctx->err_str,
      session_ctx->err_num, session_ctx->err_ind);

    Py_XDECREF(result_varlist);
    return NULL;
  }

  py_log_msg(DEBUG, "%s: End cleanup", op_name);

  py_log_msg(DEBUG, "%s: Returning %d objects", op_name, PySequence_Length(result_varlist));

  return Py_BuildValue("N", result_varlist);
}

static PyObject *netsnmp_bulkwalk(PyObject *self, PyObject *args) {
  PyObject *session = NULL;
  session_capsule_ctx *session_ctx = NULL;
  snmp_op_data op_data;
  PyObject *result_varlist = NULL;
  netsnmp_variable_list *vars = NULL;
  int error = 0;
  int op_data_error = 0;
  char *op_name = "netsnmp_bulkwalk";
  int nonrepeaters;
  int maxrepetitions;

  snmp_op_data_reset(&op_data);

  py_log_msg(DEBUG, "%s: Starting", op_name);

  if (!args) {
    const char *err_msg = "%s: missing arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto done;
  }

  if (!PyArg_ParseTuple(args, "OOii", &session, &op_data.varlist, &nonrepeaters,
                        &maxrepetitions)) {
    const char *err_msg = "%s: Could not parse arguments";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Arguments parsed", op_name);

  if (!PyList_Check(op_data.varlist)) {
    const char *err_msg = "%s: varlist is not a list";
    PyErr_Format(PyExc_ValueError, err_msg, op_name);
    error = 1;
    goto exception;
  }

  py_log_msg(DEBUG, "%s: Getting session context", op_name);
  session_ctx = get_session_context(session);

  if (!session_ctx) {
    error = 1;
    goto exception;
  }
  py_log_msg(DEBUG, "%s: Got session context", op_name);

  py_log_msg(DEBUG, "%s: Loading operation data", op_name);
  op_data.op_name = op_name;
  op_data_error = snmp_op_data_load(&op_data, session_ctx->best_guess);

  if (op_data_error || PyErr_Occurred()) {
    if(PyErr_Occurred()) {
      goto exception;
    }

    error = 1;
    goto done;
  }
  py_log_msg(DEBUG, "%s: Finished loading operation data", op_name);

  py_log_msg(DEBUG, "%s: Starting snmp request", op_name);

  result_varlist = PyList_New(0);
  PyObject * varbind = NULL;
  int notdone = 0;
  int status = 0;
  int varlist_ind = 0;
  int varlist_len = op_data.varlist_len;

  while (varlist_ind < varlist_len) {
    op_data.pdu = snmp_pdu_create(SNMP_MSG_GETBULK);
    op_data.pdu->non_repeaters = nonrepeaters;
    op_data.pdu->max_repetitions = maxrepetitions;
    snmp_add_null_var(op_data.pdu, op_data.oid_arr[varlist_ind], op_data.oid_arr_len[varlist_ind]);

    py_log_msg(DEBUG, "%s: filling request: oid(%s) "
                      "oid_idx(%s) oid_arr_len(%d) best_guess(%d)",
                op_data.op_name,
                op_data.oid_str_arr[varlist_ind],
                op_data.oid_idx_str_arr[varlist_ind],
                op_data.oid_arr_len[varlist_ind],
                session_ctx->best_guess);

    notdone = 1;
    while (notdone) {
      py_log_msg(DEBUG, "%s: Sending pdu req",
        op_data.op_name);

      status = send_pdu_request(session_ctx, &op_data);
      if((status != STAT_SUCCESS) || PyErr_Occurred()) {

        if(PyErr_Occurred()) {
          goto exception;
        }

        py_log_msg(ERROR, "%s: PDU req resulted in error Request Status(%d)",
          op_data.op_name, status);
        error = 1;
        goto done;
      }

      if (notdone) {
        vars = op_data.response->variables;
        op_data.initial_oid = op_data.initial_oid_str_arr[varlist_ind];

        while (vars) {

          if (vars->type == SNMP_ENDOFMIBVIEW) {
            py_log_msg(DEBUG, "%s: encountered end condition "
                              "(ENDOFMIBVIEW)", op_name);
            notdone = 0;
            break;
          }
          else if ((vars->type == SNMP_NOSUCHOBJECT) ||
          (vars->type == SNMP_NOSUCHINSTANCE)) {
            char val_type_str[MAX_TYPE_NAME_LEN];

            __get_type_str(vars->type, val_type_str, 1);
            varbind = py_netsnmp_construct_varbind();

            py_netsnmp_attr_set_string(varbind, "root_oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "oid", op_data.initial_oid, STRLEN(op_data.initial_oid));
            py_netsnmp_attr_set_string(varbind, "snmp_type", val_type_str,
                                       strlen(val_type_str));

            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);

            notdone = 0;
            break;
          }
          else if ((vars->name_length < op_data.oid_arr_len[varlist_ind]) ||
              (memcmp(op_data.oid_arr[varlist_ind], vars->name,
                      op_data.oid_arr_len[varlist_ind] * sizeof(oid)) != 0)) {
            py_log_msg(DEBUG, "%s: encountered end condition (next subtree iteration out of scope) var_len: %d/op_var_len: %d var_name_size: %d/op_name_size: %d",
              op_name, vars->name_length, op_data.oid_arr_len[varlist_ind],
              sizeof(op_data.oid_arr[varlist_ind]),
              sizeof(vars->name)
            );
            notdone = 0;
            break;
          }

          varbind = read_variable(vars, &op_data, session_ctx->getlabel_flag, session_ctx->sprintval_flag);

          if (varbind) {
            PyList_Append(result_varlist, varbind);
            Py_DECREF(varbind);
          } else {
            py_log_msg(ERROR, "%s bad varbind (%d)", op_name, varlist_ind);
          }

          // Create next request if we've reached the end
          if (vars->next_variable == NULL) {
            op_data.pdu = snmp_pdu_create(SNMP_MSG_GETBULK);
            op_data.pdu->non_repeaters = nonrepeaters;
            op_data.pdu->max_repetitions = maxrepetitions;
            snmp_add_null_var(op_data.pdu, vars->name, vars->name_length);
            py_log_msg(DEBUG,
              "%s: Creating pdu request for %s",
              op_name, op_data.initial_oid
            );
          }

          // Move on to next
          vars = vars->next_variable;
        }

        py_log_msg(DEBUG,
          "%s: Partially finished reading all variables from request for %s",
          op_name, op_data.initial_oid
        );
      }

      if (op_data.response) {
        snmp_free_pdu(op_data.response);
        op_data.response = NULL;
        py_log_msg(DEBUG,
          "%s: Finished reading all variables from request for %s",
          op_name, op_data.initial_oid
        );
      }
    }
    varlist_ind++;
  }

  py_log_msg(DEBUG, "%s: Finished reading all variables",op_name);
  goto done;

exception:
  snmp_op_data_finish(&op_data);
  return NULL;

done:
  py_log_msg(DEBUG, "%s: Starting cleanup", op_name);

  snmp_op_data_finish(&op_data);

  if(error) {
    py_log_msg(ERROR, "%s: Exiting due to error %d", op_name, error);

    __py_netsnmp_update_session_errors(session, session_ctx->err_str,
      session_ctx->err_num, session_ctx->err_ind);

    Py_XDECREF(result_varlist);
    return NULL;
  }

  py_log_msg(DEBUG, "%s: End cleanup", op_name);

  py_log_msg(DEBUG, "%s: Returning %d objects", op_name, PySequence_Length(result_varlist));

  return Py_BuildValue("N", result_varlist);
}

static PyObject *netsnmp_set(PyObject *self, PyObject *args) {
  PyObject *session = NULL;
  PyObject *varlist = NULL;
  PyObject *varbind = NULL;
  PyObject *ret = NULL;

  session_capsule_ctx *session_ctx = NULL;
  netsnmp_session *ss = NULL;
  netsnmp_pdu *pdu = NULL;
  netsnmp_pdu *response = NULL;

  struct tree *tp = NULL;
  char *tag = NULL;
  char *iid = NULL;
  char *val = NULL;
  char *type_str = NULL;
  int len;
  oid *oid_arr = calloc(MAX_OID_LEN, sizeof(oid));
  int oid_arr_len = MAX_OID_LEN;
  int type;
  u_char tmp_val_str[STR_BUF_SIZE];
  int use_enums;
  struct enum_list *ep = NULL;
  int best_guess;
  int status;
  int err_ind;
  int err_num;
  char err_str[STR_BUF_SIZE];
  char *tmpstr = NULL;
  Py_ssize_t tmplen;
  int error = 0;

  if (oid_arr && args) {
    if (!PyArg_ParseTuple(args, "OO", &session, &varlist)) {
      goto done;
    }

    session_ctx = get_session_context(session);

    if (!session_ctx) {
      goto done;
    }

    ss = session_ctx->handle;

    if (py_netsnmp_attr_string(session, "error_string", &tmpstr, &tmplen) < 0) {
      goto done;
    }

    use_enums = py_netsnmp_attr_long(session, "use_enums");
    best_guess = py_netsnmp_attr_long(session, "best_guess");

    pdu = snmp_pdu_create(SNMP_MSG_SET);

    if (varlist) {
      PyObject *varlist_iter = PyObject_GetIter(varlist);

      while (varlist_iter && (varbind = PyIter_Next(varlist_iter))) {
        if (py_netsnmp_attr_string(varbind, "oid", &tag, NULL) < 0 ||
            py_netsnmp_attr_string(varbind, "oid_index", &iid, NULL) < 0) {
          oid_arr_len = 0;
        } else {
          tp = __tag2oid(tag, iid, oid_arr, &oid_arr_len, &type, best_guess);
        }

        if (oid_arr_len == 0) {
          PyErr_Format(EasySNMPUnknownObjectIDError, "unknown object id (%s)",
                       (tag ? tag : "<null>"));
          error = 1;
          snmp_free_pdu(pdu);
          Py_DECREF(varbind);
          Py_DECREF(varlist_iter);
          goto done;
        }

        if (type == TYPE_UNKNOWN) {
          if (py_netsnmp_attr_string(varbind, "snmp_type", &type_str, NULL) <
              0) {
            snmp_free_pdu(pdu);
            Py_DECREF(varbind);
            Py_DECREF(varlist_iter);
            goto done;
          }
          type = __translate_appl_type(type_str);
          if (type == TYPE_UNKNOWN) {
            PyErr_SetString(EasySNMPUndeterminedTypeError,
                            "a type could not be determine for "
                            "the object");
            error = 1;
            snmp_free_pdu(pdu);
            Py_DECREF(varbind);
            Py_DECREF(varlist_iter);
            goto done;
          }
        }

        if (py_netsnmp_attr_string(varbind, "value", &val, &tmplen) < 0) {
          snmp_free_pdu(pdu);
          Py_DECREF(varbind);
          Py_DECREF(varlist_iter);
          goto done;
        }

        memset(tmp_val_str, 0, sizeof(tmp_val_str));
        if (tmplen >= sizeof(tmp_val_str)) {
          tmplen = sizeof(tmp_val_str) - 1;
        }

        memcpy(tmp_val_str, val, tmplen);
        if (type == TYPE_INTEGER && use_enums && tp && tp->enums) {
          for (ep = tp->enums; ep; ep = ep->next) {
            if (val && !strcmp(ep->label, val)) {
              snprintf((char *)tmp_val_str, sizeof(tmp_val_str), "%d",
                       ep->value);
              break;
            }
          }
        }
        len = (int)tmplen;
        status = __add_var_val_str(pdu, oid_arr, oid_arr_len,
                                   (char *)tmp_val_str, len, type);

        if (status == FAILURE) {
          py_log_msg(ERROR, "set: adding variable/value to PDU");
        }

        /* release reference when done */
        Py_DECREF(varbind);
      }

      Py_DECREF(varlist_iter);

      if (PyErr_Occurred()) {
        error = 1;
        snmp_free_pdu(pdu);
        goto done;
      }
    }

    status = __send_sync_pdu(ss, pdu, &response, NO_RETRY_NOSUCH, err_str,
                             &err_num, &err_ind, NULL);
    __py_netsnmp_update_session_errors(session, err_str, err_num, err_ind);
    if (status != 0) {
      error = 1;
      goto done;
    }

    if (response) {
      snmp_free_pdu(response);
    }

    if (status == STAT_SUCCESS) {
      ret = Py_BuildValue("i", 1); /* success, return True */
    } else {
      ret = Py_BuildValue("i", 0); /* fail, return False */
    }
  }

done:

  Py_XDECREF(varbind);
  SAFE_FREE(oid_arr);
  if (error) {
    return NULL;
  } else {
    return (ret ? ret : Py_BuildValue(""));
  }
}

/**
 * Get a logger object from the logging module.
 */
static PyObject *py_get_logger(char *logger_name) {
  PyObject *logger = NULL;
  PyObject *null_handler = NULL;

  logger = PyObject_CallMethod(logging_import, "getLogger", "s", logger_name);
  if (logger == NULL) {
    const char *err_msg = "failed to call logging.getLogger";
    PyErr_SetString(PyExc_RuntimeError, err_msg);
    goto done;
  }

  /*
   * Since this is a library module, a handler needs to be configured when
   * logging; otherwise a warning is emitted to stderr.
   *
   * https://docs.python.org/3.4/howto/logging.html#library-config recommends:
   * >>> logging.getLogger('foo').addHandler(logging.NullHandler())
   *
   * However NullHandler doesn't come with python <2.6 and <3.1, so we need
   * to improvise by using an identical copy in easysnmp.compat.
   *
   */

  null_handler =
      PyObject_CallMethod(easysnmp_compat_import, "NullHandler", NULL);
  if (null_handler == NULL) {
    const char *err_msg = "failed to call easysnmp.compat.NullHandler()";
    PyErr_SetString(PyExc_RuntimeError, err_msg);
    goto done;
  }

  if (PyObject_CallMethod(logger, "addHandler", "O", null_handler) == NULL) {
    const char *err_msg = "failed to call logger.addHandler(NullHandler())";
    PyErr_SetString(PyExc_RuntimeError, err_msg);
    goto done;
  }

  /* we don't need the null_handler around anymore. */
  Py_DECREF(null_handler);

  return logger;

done:

  Py_XDECREF(logger);
  Py_XDECREF(null_handler);

  return NULL;
}

static void py_log_msg(int log_level, char *printf_fmt, ...) {
  PyObject *log_msg = NULL;
  va_list fmt_args;

  va_start(fmt_args, printf_fmt);
  log_msg = PyUnicode_FromFormatV(printf_fmt, fmt_args);
  va_end(fmt_args);

  if (log_msg == NULL) {
    /* fail silently. */
    return;
  }

  /* call function depending on loglevel */
  switch (log_level) {
  case INFO:
    PyObject_CallMethod(PyLogger, "info", "O", log_msg);
    break;

  case WARNING:
    PyObject_CallMethod(PyLogger, "warn", "O", log_msg);
    break;

  case ERROR:
    PyObject_CallMethod(PyLogger, "error", "O", log_msg);
    break;

  case DEBUG:
    PyObject_CallMethod(PyLogger, "debug", "O", log_msg);
    break;

  case EXCEPTION:
    PyObject_CallMethod(PyLogger, "exception", "O", log_msg);
    break;

  default:
    break;
  }

  Py_DECREF(log_msg);
}

/*
 * Array of defined methods when initialising the module,
 * each entry must contain the following:
 *
 *     (char *)      ml_name:   name of method
 *     (PyCFunction) ml_meth:   pointer to the C implementation
 *     (int)         ml_flags:  flag bit indicating how call should be
 *     (char *)      ml_doc:    points to contents of method docstring
 *
 * See: https://docs.python.org/2/c-api/structures.html for more info.
 *
 */
static PyMethodDef interface_methods[] = {
    {"session", netsnmp_create_session, METH_VARARGS,
     "create a netsnmp session."},
    {"session_v3", netsnmp_create_session_v3, METH_VARARGS,
     "create a netsnmp session."},
    {"session_tunneled", netsnmp_create_session_tunneled, METH_VARARGS,
     "create a tunneled netsnmp session over tls, dtls or ssh."},
    {"get", netsnmp_get, METH_VARARGS, "perform an SNMP GET operation."},
    {"getnext", netsnmp_getnext, METH_VARARGS,
     "perform an SNMP GETNEXT operation."},
    {"getbulk", netsnmp_getbulk, METH_VARARGS,
     "perform an SNMP GETBULK operation."},
    {"set", netsnmp_set, METH_VARARGS, "perform an SNMP SET operation."},
    {"walk", netsnmp_walk, METH_VARARGS, "perform an SNMP WALK operation."},
    {"bulkwalk", netsnmp_bulkwalk, METH_VARARGS,
     "perform an SNMP BULKWALK operation."},
    {NULL, NULL, 0, NULL} /* Sentinel */
};

/* entry point when importing the module */
#if PY_MAJOR_VERSION >= 3

static struct PyModuleDef moduledef = {PyModuleDef_HEAD_INIT, "interface", NULL,
                                       -1, interface_methods};

PyMODINIT_FUNC PyInit_interface(void) {
  /* Initialise the module */
  PyObject *interface_module = PyModule_Create(&moduledef);

#else

PyMODINIT_FUNC initinterface(void) {
  /* Initialise the module */
  PyObject *interface_module = Py_InitModule("interface", interface_methods);

#endif
  if (interface_module == NULL) {
    goto done;
  }

  /*
   * Perform global imports:
   *
   * import logging
   * import easysnmp
   * import easysnmp.exceptions
   * import easysnmp.compat
   *
   */
  logging_import = PyImport_ImportModule("logging");
  if (logging_import == NULL) {
    const char *err_msg = "failed to import 'logging'";
    PyErr_SetString(PyExc_ImportError, err_msg);
    goto done;
  }

  easysnmp_import = PyImport_ImportModule("easysnmp");
  if (easysnmp_import == NULL) {
    const char *err_msg = "failed to import 'easysnmp'";
    PyErr_SetString(PyExc_ImportError, err_msg);
    goto done;
  }

  easysnmp_exceptions_import = PyImport_ImportModule("easysnmp.exceptions");
  if (easysnmp_exceptions_import == NULL) {
    const char *err_msg = "failed to import 'easysnmp.exceptions'";
    PyErr_SetString(PyExc_ImportError, err_msg);
    goto done;
  }

  easysnmp_compat_import = PyImport_ImportModule("easysnmp.compat");
  if (easysnmp_compat_import == NULL) {
    const char *err_msg = "failed to import 'easysnmp.compat'";
    PyErr_SetString(PyExc_ImportError, err_msg);
    goto done;
  }

  EasySNMPError =
      PyObject_GetAttrString(easysnmp_exceptions_import, "EasySNMPError");
  EasySNMPConnectionError = PyObject_GetAttrString(easysnmp_exceptions_import,
                                                   "EasySNMPConnectionError");
  EasySNMPTimeoutError = PyObject_GetAttrString(easysnmp_exceptions_import,
                                                "EasySNMPTimeoutError");
  EasySNMPNoSuchNameError = PyObject_GetAttrString(easysnmp_exceptions_import,
                                                   "EasySNMPNoSuchNameError");
  EasySNMPUnknownObjectIDError = PyObject_GetAttrString(
      easysnmp_exceptions_import, "EasySNMPUnknownObjectIDError");
  EasySNMPNoSuchObjectError = PyObject_GetAttrString(
      easysnmp_exceptions_import, "EasySNMPNoSuchObjectError");
  EasySNMPNoSuchInstanceError = PyObject_GetAttrString(
      easysnmp_exceptions_import, "EasySNMPNoSuchInstanceError");
  EasySNMPUndeterminedTypeError = PyObject_GetAttrString(
      easysnmp_exceptions_import, "EasySNMPUndeterminedTypeError");

  /* Initialise logging (note: automatically has refcount 1) */
  PyLogger = py_get_logger("easysnmp.interface");

  if (PyLogger == NULL) {
    goto done;
  }

  /* initialise the netsnmp library */
  __libraries_init("python");

  py_log_msg(DEBUG, "initialised easysnmp.interface");

#if PY_MAJOR_VERSION >= 3
  return interface_module;
#else
  return;
#endif

done:
  Py_XDECREF(interface_module);
  Py_XDECREF(logging_import);
  Py_XDECREF(easysnmp_import);
  Py_XDECREF(easysnmp_exceptions_import);
  Py_XDECREF(easysnmp_compat_import);
  Py_XDECREF(EasySNMPError);
  Py_XDECREF(EasySNMPConnectionError);
  Py_XDECREF(EasySNMPTimeoutError);
  Py_XDECREF(EasySNMPNoSuchNameError);
  Py_XDECREF(EasySNMPUnknownObjectIDError);
  Py_XDECREF(EasySNMPNoSuchObjectError);
  Py_XDECREF(EasySNMPNoSuchInstanceError);
  Py_XDECREF(EasySNMPUndeterminedTypeError);
  Py_XDECREF(PyLogger);

#if PY_MAJOR_VERSION >= 3
  return NULL;
#else
  return;
#endif
}
