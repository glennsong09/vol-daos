/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have          *
 * access to either file, you may request a copy from help@hdfgroup.org.     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Programmer:  Neil Fortner <nfortne2@hdfgroup.org>
 *              September, 2016
 *
 * Purpose: The DAOS VOL plugin where access is forwarded to the DAOS library.
 */

#include "H5public.h"           /* Generic Functions                    */
#include "H5Dpublic.h"          /* Datasets                             */
#include "H5Epublic.h"          /* Error handling                       */
#include "H5Fpublic.h"          /* Files                                */
#include "H5FDpublic.h"         /* File drivers                         */
#include "H5Ipublic.h"          /* IDs                                  */
#include "H5Opublic.h"          /* Objects                              */
#include "H5Ppublic.h"          /* Property lists                       */
#include "H5PLpublic.h"         /* Plugins                              */
#include "H5Spublic.h"          /* Dataspaces                           */
#include "H5VLpublic.h"         /* VOL plugins                          */

#include "daos_vol.h"           /* DAOS plugin                          */
#include "daos_vol_err.h"       /* DAOS plugin error handling           */
#include "daos_vol_config.h"    /* DAOS plugin configuration header     */

#include "util/daos_vol_mem.h"  /* DAOS plugin memory management        */

hid_t H5_DAOS_g = -1;

/* Identifiers for HDF5's error API */
hid_t dv_err_stack_g = -1;
hid_t dv_err_class_g = -1;

/* DSINC - Exclude map functionality for now */
#undef DV_HAVE_MAP

/* DSINC - There are serious problems in HDF5 when trying to call
 * H5Pregister2/H5Punregister on the H5P_FILE_ACCESS class.
 */
#undef DV_HAVE_SNAP_OPEN_ID

/* DSINC - Link and attribute iteration can't be supported until
 * problems with needing to pass the iteration's root object ID
 * can be solved.
 */
#undef DV_HAVE_LINK_ITERATION
#undef DV_HAVE_ATTR_ITERATION

#ifdef DV_TRACK_MEM_USAGE
/*
 * Counter to keep track of the currently allocated amount of bytes
 */
static size_t daos_vol_curr_alloc_bytes;
#endif

/*
 * Macros
 */
/* Constant Keys */
#define H5_DAOS_INT_MD_KEY "/Internal Metadata"
#define H5_DAOS_MAX_OID_KEY "Max OID"
#define H5_DAOS_CPL_KEY "Creation Property List"
#define H5_DAOS_LINK_KEY "Link"
#define H5_DAOS_TYPE_KEY "Datatype"
#define H5_DAOS_SPACE_KEY "Dataspace"
#define H5_DAOS_ATTR_KEY "/Attribute"
#define H5_DAOS_CHUNK_KEY 0u

#ifdef DV_HAVE_MAP
#define H5_DAOS_KTYPE_KEY "Key Datatype"
#define H5_DAOS_VTYPE_KEY "Value Datatype"
#define H5_DAOS_MAP_KEY "MAP_AKEY"
#endif

/* Stack allocation sizes */
#define H5_DAOS_GH_BUF_SIZE 1024
#define H5_DAOS_FOI_BUF_SIZE 1024
#define H5_DAOS_LINK_VAL_BUF_SIZE 256
#define H5_DAOS_GINFO_BUF_SIZE 256
#define H5_DAOS_DINFO_BUF_SIZE 1024
#define H5_DAOS_TINFO_BUF_SIZE 1024
#define H5_DAOS_SEQ_LIST_LEN 128
#define H5_DAOS_ITER_LEN 128
#define H5_DAOS_ITER_SIZE_INIT (4 * 1024)

/* Definitions for building oids */
#define H5_DAOS_IDX_MASK   0x3fffffffffffffffull
#define H5_DAOS_TYPE_MASK  0xc000000000000000ull
#define H5_DAOS_TYPE_GRP   0x0000000000000000ull
#define H5_DAOS_TYPE_DSET  0x4000000000000000ull
#define H5_DAOS_TYPE_DTYPE 0x8000000000000000ull
#define H5_DAOS_TYPE_MAP   0xc000000000000000ull

/* Macros borrowed from H5Fprivate.h */
#define UINT64ENCODE(p, n) {                           \
   uint64_t _n = (n);                                  \
   size_t _i;                                          \
   uint8_t *_p = (uint8_t*)(p);                        \
                                                       \
   for (_i = 0; _i < sizeof(uint64_t); _i++, _n >>= 8) \
      *_p++ = (uint8_t)(_n & 0xff);                    \
   for (/*void*/; _i < 8; _i++)                        \
      *_p++ = 0;                                       \
   (p) = (uint8_t*)(p) + 8;                            \
}

#define UINT64DECODE(p, n) {                 \
   /* WE DON'T CHECK FOR OVERFLOW! */        \
   size_t _i;                                \
                                             \
   n = 0;                                    \
   (p) += 8;                                 \
   for (_i = 0; _i < sizeof(uint64_t); _i++) \
      n = (n << 8) | *(--p);                 \
   (p) += 8;                                 \
}

/* Decode a variable-sized buffer */
/* (Assumes that the high bits of the integer will be zero) */
#define DECODE_VAR(p, n, l) { \
   size_t _i;                 \
                              \
   n = 0;                     \
   (p) += l;                  \
   for (_i = 0; _i < l; _i++) \
      n = (n << 8) | *(--p);  \
   (p) += l;                  \
}

/* Decode a variable-sized buffer into a 64-bit unsigned integer */
/* (Assumes that the high bits of the integer will be zero) */
#define UINT64DECODE_VAR(p, n, l)     DECODE_VAR(p, n, l)

/* Macro borrowed from H5private.h for defining the _ATTR_UNUSED macro */
#ifdef __cplusplus
#   define DV_ATTR_UNUSED       /*void*/
#else /* __cplusplus */
#if defined(H5_HAVE_ATTRIBUTE) && !defined(__SUNPRO_C)
#   define DV_ATTR_UNUSED       __attribute__((unused))
#else
#   define DV_ATTR_UNUSED       /*void*/
#endif
#endif /* __cplusplus */

/* DAOS-specific file access properties */
typedef struct H5_daos_fapl_t {
    MPI_Comm            comm;           /* communicator                  */
    MPI_Info            info;           /* file information              */
} H5_daos_fapl_t;

/* Enum to indicate if the supplied read buffer can be used as a type conversion
 * or background buffer */
typedef enum {
    H5_DAOS_TCONV_REUSE_NONE,    /* Cannot reuse buffer */
    H5_DAOS_TCONV_REUSE_TCONV,   /* Use buffer as type conversion buffer */
    H5_DAOS_TCONV_REUSE_BKG      /* Use buffer as background buffer */
} H5_daos_tconv_reuse_t;

/* Udata type for H5Dscatter callback */
typedef struct H5_daos_scatter_cb_ud_t {
    void *buf;
    size_t len;
} H5_daos_scatter_cb_ud_t;

/* Udata type for memory space H5Diterate callback */
typedef struct {
    daos_iod_t *iods;
    daos_sg_list_t *sgls;
    daos_iov_t *sg_iovs;
    hbool_t is_vl_str;
    size_t base_type_size;
    uint64_t offset;
    uint64_t idx;
} H5_daos_vl_mem_ud_t;

/* Udata type for file space H5Diterate callback */
typedef struct {
    uint8_t **akeys;
    daos_iod_t *iods;
    uint64_t idx;
} H5_daos_vl_file_ud_t;

/* Prototypes */
static void *H5_daos_fapl_copy(const void *_old_fa);
static herr_t H5_daos_fapl_free(void *_fa);
static herr_t H5_daos_term(void);

/* File callbacks */
static void *H5_daos_file_create(const char *name, unsigned flags,
    hid_t fcpl_id, hid_t fapl_id, hid_t dxpl_id, void **req);
static void *H5_daos_file_open(const char *name, unsigned flags,
    hid_t fapl_id, hid_t dxpl_id, void **req);
/* static herr_t H5_daos_file_get(void *file, H5VL_file_get_t get_type, hid_t dxpl_id, void **req, va_list arguments); */
static herr_t H5_daos_file_specific(void *_item,
    H5VL_file_specific_t specific_type, hid_t dxpl_id, void **req,
    va_list arguments);
static herr_t H5_daos_file_close(void *_file, hid_t dxpl_id, void **req);

/* Link callbacks */
static herr_t H5_daos_link_create(H5VL_link_create_type_t create_type,
    void *_item, const H5VL_loc_params_t *loc_params, hid_t lcpl_id, hid_t lapl_id,
    hid_t dxpl_id, void **req);
static herr_t H5_daos_link_specific(void *_item,
    const H5VL_loc_params_t *loc_params, H5VL_link_specific_t specific_type,
    hid_t dxpl_id, void **req, va_list arguments);

/* Group callbacks */
static void *H5_daos_group_create(void *_item, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t gcpl_id, hid_t gapl_id, hid_t dxpl_id, void **req);
static void *H5_daos_group_open(void *_item, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t gapl_id, hid_t dxpl_id, void **req);
static herr_t H5_daos_group_close(void *_grp, hid_t dxpl_id, void **req);

/* Dataset callbacks */
static void *H5_daos_dataset_create(void *_item,
    const H5VL_loc_params_t *loc_params, const char *name, hid_t dcpl_id,
    hid_t dapl_id, hid_t dxpl_id, void **req);
static void *H5_daos_dataset_open(void *_item, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t dapl_id, hid_t dxpl_id, void **req);
static herr_t H5_daos_dataset_read(void *_dset, hid_t mem_type_id,
    hid_t mem_space_id, hid_t file_space_id, hid_t dxpl_id, void *buf,
    void **req);
static herr_t H5_daos_dataset_write(void *_dset, hid_t mem_type_id,
    hid_t mem_space_id, hid_t file_space_id, hid_t dxpl_id, const void *buf,
    void **req);
/* static herr_t H5_daos_dataset_specific(void *_dset, H5VL_dataset_specific_t specific_type,
                                        hid_t dxpl_id, void **req, va_list arguments);*/
static herr_t H5_daos_dataset_get(void *_dset, H5VL_dataset_get_t get_type,
    hid_t dxpl_id, void **req, va_list arguments);
static herr_t H5_daos_dataset_close(void *_dset, hid_t dxpl_id, void **req);

/* Datatype callbacks */
static void *H5_daos_datatype_commit(void *obj, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t type_id, hid_t lcpl_id, hid_t tcpl_id,
    hid_t tapl_id, hid_t dxpl_id, void **req);
static void *H5_daos_datatype_open(void *_item, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t tapl_id, hid_t dxpl_id, void **req);
static herr_t H5_daos_datatype_get(void *obj, H5VL_datatype_get_t get_type,
    hid_t dxpl_id, void **req, va_list arguments);

/* Object callbacks */
static void *H5_daos_object_open(void *_item, const H5VL_loc_params_t *loc_params,
    H5I_type_t *opened_type, hid_t dxpl_id, void **req);
static herr_t H5_daos_object_optional(void *_item, hid_t dxpl_id, void **req,
    va_list arguments);

/* Attribute callbacks */
static void *H5_daos_attribute_create(void *_obj,
    const H5VL_loc_params_t *loc_params, const char *name, hid_t acpl_id,
    hid_t aapl_id, hid_t dxpl_id, void **req);
static void *H5_daos_attribute_open(void *_obj, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t aapl_id, hid_t dxpl_id, void **req);
static herr_t H5_daos_attribute_read(void *_attr, hid_t mem_type_id,
    void *buf, hid_t dxpl_id, void **req);
static herr_t H5_daos_attribute_write(void *_attr, hid_t mem_type_id,
    const void *buf, hid_t dxpl_id, void **req);
static herr_t H5_daos_attribute_get(void *_item, H5VL_attr_get_t get_type,
    hid_t dxpl_id, void **req, va_list arguments);
static herr_t H5_daos_attribute_specific(void *_item,
    const H5VL_loc_params_t *loc_params, H5VL_attr_specific_t specific_type,
    hid_t dxpl_id, void **req, va_list arguments);
static herr_t H5_daos_attribute_close(void *_attr, hid_t dxpl_id,
    void **req);

/* Helper routines */
static herr_t H5_daos_write_max_oid(H5_daos_file_t *file);
static herr_t H5_daos_file_flush(H5_daos_file_t *file);
static herr_t H5_daos_file_close_helper(H5_daos_file_t *file,
    hid_t dxpl_id, void **req);

static herr_t H5_daos_link_read(H5_daos_group_t *grp, const char *name,
    size_t name_len, H5_daos_link_val_t *val);
static herr_t H5_daos_link_write(H5_daos_group_t *grp, const char *name,
    size_t name_len, H5_daos_link_val_t *val);
static herr_t H5_daos_link_follow(H5_daos_group_t *grp, const char *name,
    size_t name_len, hid_t dxpl_id, void **req, daos_obj_id_t *oid);

static H5_daos_group_t *H5_daos_group_traverse(H5_daos_item_t *item,
    const char *path, hid_t dxpl_id, void **req, const char **obj_name,
    void **gcpl_buf_out, uint64_t *gcpl_len_out);
static void *H5_daos_group_create_helper(H5_daos_file_t *file,
    hid_t gcpl_id, hid_t gapl_id, hid_t dxpl_id, void **req,
    H5_daos_group_t *parent_grp, const char *name, size_t name_len,
    hbool_t collective);
static void *H5_daos_group_open_helper(H5_daos_file_t *file,
    daos_obj_id_t oid, hid_t gapl_id, hid_t dxpl_id, void **req,
    void **gcpl_buf_out, uint64_t *gcpl_len_out);
static void *H5_daos_group_reconstitute(H5_daos_file_t *file,
    daos_obj_id_t oid, uint8_t *gcpl_buf, hid_t gapl_id, hid_t dxpl_id,
    void **req);

static htri_t H5_daos_need_bkg(hid_t src_type_id, hid_t dst_type_id,
    size_t *dst_type_size, hbool_t *fill_bkg);
static herr_t H5_daos_tconv_init(hid_t src_type_id, size_t *src_type_size,
    hid_t dst_type_id, size_t *dst_type_size, size_t num_elem, void **tconv_buf,
    void **bkg_buf, H5_daos_tconv_reuse_t *reuse, hbool_t *fill_bkg);
static herr_t H5_daos_sel_to_recx_iov(hid_t space_id, size_t type_size,
    void *buf, daos_recx_t **recxs, daos_iov_t **sg_iovs, size_t *list_nused);
static herr_t H5_daos_scatter_cb(const void **src_buf,
    size_t *src_buf_bytes_used, void *_udata);
static herr_t H5_daos_dataset_mem_vl_rd_cb(void *_elem, hid_t type_id,
    unsigned ndim, const hsize_t *point, void *_udata);
static herr_t H5_daos_dataset_file_vl_cb(void *_elem, hid_t type_id,
    unsigned ndim, const hsize_t *point, void *_udata);
static herr_t H5_daos_dataset_mem_vl_wr_cb(void *_elem, hid_t type_id,
    unsigned ndim, const hsize_t *point, void *_udata);

static herr_t H5_daos_datatype_close(void *_dtype, hid_t dxpl_id,
    void **req);

static herr_t H5_daos_object_close(void *_obj, hid_t dxpl_id, void **req);

/* Free list definitions */
/* DSINC - currently no external access to free lists
H5FL_DEFINE(H5_daos_file_t);
H5FL_DEFINE(H5_daos_group_t);
H5FL_DEFINE(H5_daos_dset_t);
H5FL_DEFINE(H5_daos_dtype_t);
H5FL_DEFINE(H5_daos_map_t);
H5FL_DEFINE(H5_daos_attr_t);*/

/* DSINC - Until we determine what to do with free lists,
 * these macros should at least keep the allocations working
 * correctly.
 */
#define H5FL_CALLOC(t) DV_calloc(sizeof(t))
#define H5FL_FREE(t, o) DV_free(o)

/* The DAOS VOL plugin struct */
static H5VL_class_t H5_daos_g = {
    HDF5_VOL_DAOS_VERSION_1,                 /* Plugin Version number */
    H5_VOL_DAOS_CLS_VAL,                     /* Plugin Value */
    H5_DAOS_VOL_NAME,                        /* Plugin Name */
    0,                                       /* Plugin capability flags */
    H5_daos_init,                            /* Plugin initialize */
    H5_daos_term,                            /* Plugin terminate */
    sizeof(H5_daos_fapl_t),                  /* Plugin Info size */
    H5_daos_fapl_copy,                       /* Plugin Info copy */
    NULL,                                    /* Plugin Info compare */
    H5_daos_fapl_free,                       /* Plugin Info free */
    NULL,                                    /* Plugin Info To String */
    NULL,                                    /* Plugin String To Info */
    NULL,                                    /* Plugin Get Object */
    NULL,                                    /* Plugin Get Wrap Ctx */
    NULL,                                    /* Plugin Wrap Object */
    NULL,                                    /* Plugin Free Wrap Ctx */
    {                                        /* Plugin Attribute cls */
        H5_daos_attribute_create,            /* Plugin Attribute create */
        H5_daos_attribute_open,              /* Plugin Attribute open */
        H5_daos_attribute_read,              /* Plugin Attribute read */
        H5_daos_attribute_write,             /* Plugin Attribute write */
        H5_daos_attribute_get,               /* Plugin Attribute get */
        H5_daos_attribute_specific,          /* Plugin Attribute specific */
        NULL,                                /* Plugin Attribute optional */
        H5_daos_attribute_close              /* Plugin Attribute close */
    },
    {                                        /* Plugin Dataset cls */
        H5_daos_dataset_create,              /* Plugin Dataset create */
        H5_daos_dataset_open,                /* Plugin Dataset open */
        H5_daos_dataset_read,                /* Plugin Dataset read */
        H5_daos_dataset_write,               /* Plugin Dataset write */
        H5_daos_dataset_get,                 /* Plugin Dataset get */
        NULL,/*H5_daos_dataset_specific,*/   /* Plugin Dataset specific */
        NULL,                                /* Plugin Dataset optional */
        H5_daos_dataset_close                /* Plugin Dataset close */
    },
    {                                        /* Plugin Datatype cls */
        H5_daos_datatype_commit,             /* Plugin Datatype commit */
        H5_daos_datatype_open,               /* Plugin Datatype open */
        H5_daos_datatype_get,                /* Plugin Datatype get */
        NULL,                                /* Plugin Datatype specific */
        NULL,                                /* Plugin Datatype optional */
        H5_daos_datatype_close               /* Plugin Datatype close */
    },
    {                                        /* Plugin File cls */
        H5_daos_file_create,                 /* Plugin File create */
        H5_daos_file_open,                   /* Plugin File open */
        NULL,/*H5_daos_file_get,*/           /* Plugin File get */
        H5_daos_file_specific,               /* Plugin File specific */
        NULL,                                /* Plugin File optional */
        H5_daos_file_close                   /* Plugin File close */
    },
    {                                        /* Plugin Group cls */
        H5_daos_group_create,                /* Plugin Group create */
        H5_daos_group_open,                  /* Plugin Group open */
        NULL,/*H5_daos_group_get,*/          /* Plugin Group get */
        NULL,                                /* Plugin Group specific */
        NULL,                                /* Plugin Group optional */
        H5_daos_group_close                  /* Plugin Group close */
    },
    {                                        /* Plugin Link cls */
        H5_daos_link_create,                 /* Plugin Link create */
        NULL,/*H5_daos_link_copy,*/          /* Plugin Link copy */
        NULL,/*H5_daos_link_move,*/          /* Plugin Link move */
        NULL,/*H5_daos_link_get,*/           /* Plugin Link get */
        H5_daos_link_specific,               /* Plugin Link specific */
        NULL                                 /* Plugin Link optional */
    },
    {                                        /* Plugin Object cls */
        H5_daos_object_open,                 /* Plugin Object open */
        NULL,                                /* Plugin Object copy */
        NULL,                                /* Plugin Object get */
        NULL,/*H5_daos_object_specific,*/    /* Plugin Object specific */
        H5_daos_object_optional              /* Plugin Object optional */
    },
    {
        NULL,                                /* Plugin Request wait */
        NULL,                                /* Plugin Request notify */
        NULL,                                /* Plugin Request cancel */
        NULL,                                /* Plugin Request specific */
        NULL,                                /* Plugin Request optional */
        NULL,                                /* Plugin Request free */
    },
    NULL                                     /* Plugin optional */
};

/* Pool handle for use with all files */
daos_handle_t H5_daos_poh_g = {0}; /* Hack! use a DAOS macro if a usable one is created DSINC */

/* Global variables used to open the pool */
hbool_t pool_globals_set_g = FALSE;
MPI_Comm pool_comm_g;
uuid_t pool_uuid_g;
char *pool_grp_g = NULL;

#if 0

/*--------------------------------------------------------------------------
NAME
   H5VL__init_package -- Initialize interface-specific information
USAGE
    herr_t H5VL__init_package()

RETURNS
    Non-negative on success/Negative on failure
DESCRIPTION
    Initializes any interface-specific data or routines.  (Just calls
    H5_daos_init currently).

--------------------------------------------------------------------------*/
static herr_t
H5VL__init_package(void)
{
    herr_t ret_value = SUCCEED; 

    if(H5_daos_init() < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "unable to initialize FF DAOS VOL plugin")

done:
    D_FUNC_LEAVE
} /* H5VL__init_package() */
#endif


/*-------------------------------------------------------------------------
 * Function:    H5daos_init
 *
 * Purpose:     Initialize this VOL connector by connecting to the pool and
 *              registering the connector with the library.  pool_comm
 *              identifies the communicator used to connect to the DAOS
 *              pool.  This should include all processes that will
 *              participate in I/O.  This call is collective across
 *              pool_comm.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              March, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_init(MPI_Comm pool_comm, uuid_t pool_uuid, char *pool_grp)
{
    herr_t ret_value = SUCCEED;            /* Return value */

    /* Initialize HDF5 */
    if (H5open() < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "HDF5 failed to initialize")

    /* Register the DAOS VOL, if it isn't already */
    if(H5I_VOL != H5Iget_type(H5_DAOS_g)) {
        htri_t is_registered;

        if ((is_registered = H5VLis_connector_registered(H5_daos_g.name)) < 0)
            D_GOTO_ERROR(H5E_ATOM, H5E_CANTINIT, FAIL, "can't determine if DAOS VOL plugin is registered")

        if (!is_registered) {
            /* Save arguments to globals */
            pool_comm_g = pool_comm;
            memcpy(pool_uuid_g, pool_uuid, sizeof(uuid_t));
            pool_grp_g = pool_grp;
            pool_globals_set_g = TRUE;

            /* Register connector */
            if((H5_DAOS_g = H5VLregister_connector((const H5VL_class_t *)&H5_daos_g, H5P_DEFAULT)) < 0)
                D_GOTO_ERROR(H5E_ATOM, H5E_CANTINSERT, FAIL, "can't create ID for DAOS VOL plugin")
        } /* end if */
        else {
            if((H5_DAOS_g = H5VLget_connector_id(H5_daos_g.name)) < 0)
                D_GOTO_ERROR(H5E_ATOM, H5E_CANTGET, FAIL, "unable to get registered ID for DAOS VOL plugin")
        } /* end else */
    } /* end if */

done:
    D_FUNC_LEAVE_API
} /* end H5daos_init() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_init
 *
 * Purpose:     Initialize this VOL connector by registering the connector
 *              with the library.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_init(hid_t vipl_id)
{
#ifdef DV_HAVE_SNAP_OPEN_ID
    H5_daos_snap_id_t snap_id_default;
#endif
    int pool_rank;
    int pool_num_procs;
    daos_iov_t glob;
    uint64_t gh_buf_size;
    char gh_buf_static[H5_DAOS_GH_BUF_SIZE];
    char *gh_buf_dyn = NULL;
    char *gh_buf = gh_buf_static;
    uint8_t *p;
    hbool_t must_bcast = FALSE;
    int ret;
    herr_t ret_value = SUCCEED;            /* Return value */

    /* Register interfaces that might not be initialized in time (for example if
     * we open an object without knowing its type first, H5Oopen will not
     * initialize that type) */
    /* if(H5G_init() < 0)
        D_GOTO_ERROR(H5E_FUNC, H5E_CANTINIT, FAIL, "unable to initialize group interface")
    if(H5M_init() < 0)
        D_GOTO_ERROR(H5E_FUNC, H5E_CANTINIT, FAIL, "unable to initialize map interface")
    if(H5D_init() < 0)
        D_GOTO_ERROR(H5E_FUNC, H5E_CANTINIT, FAIL, "unable to initialize dataset interface")
    if(H5T_init() < 0)
        D_GOTO_ERROR(H5E_FUNC, H5E_CANTINIT, FAIL, "unable to initialize datatype interface") */

    if((dv_err_stack_g = H5Ecreate_stack()) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't create HDF5 error stack\n")

    /* Register the plugin with HDF5's error reporting API */
    if((dv_err_class_g = H5Eregister_class(DAOS_VOL_ERR_CLS_NAME, DAOS_VOL_ERR_LIB_NAME, DAOS_VOL_ERR_VER)) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't register with HDF5 error API")

#ifdef DV_HAVE_SNAP_OPEN_ID
    /* Register the DAOS SNAP_OPEN_ID property with HDF5 */
    snap_id_default = H5_DAOS_SNAP_ID_INVAL;
    if(H5Pregister2(H5P_FILE_ACCESS, H5_DAOS_SNAP_OPEN_ID, sizeof(H5_daos_snap_id_t), (H5_daos_snap_id_t *) &snap_id_default,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL) < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "unable to register DAOS SNAP_OPEN_ID property")
#endif

    /* Initialize daos */
    if((0 != (ret = daos_init())) && (ret != -DER_ALREADY))
        D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "DAOS failed to initialize: %d", ret)

#ifdef DV_TRACK_MEM_USAGE
    /* Initialize allocated memory counter */
    daos_vol_curr_alloc_bytes = 0;
#endif

    /* Set pool globals to default values if they were not already set */
    if(!pool_globals_set_g) {
        pool_comm_g = MPI_COMM_WORLD;
        memset(pool_uuid_g, 0, sizeof(pool_uuid_g));
        assert(!pool_grp_g);
    } /* end if */

    /* Obtain the process rank and size from the communicator attached to the
     * fapl ID */
    MPI_Comm_rank(pool_comm_g, &pool_rank);
    MPI_Comm_size(pool_comm_g, &pool_num_procs);

    if(pool_rank == 0) {
        char *svcl_str = NULL;
        daos_pool_info_t pool_info;
        d_rank_list_t *svcl = NULL;
        char *uuid_str = NULL;
        uuid_t pool_uuid;

        /* If there are other processes and we fail we must bcast anyways so they
         * don't hang */
        if(pool_num_procs > 1)
            must_bcast = TRUE;

        uuid_str = getenv ("DAOS_POOL");
        if (uuid_str != NULL) {
            if (uuid_parse(uuid_str, pool_uuid) < 0) {
                fprintf(stderr, "Failed to parse pool UUID env\n");
                return -1;
            } /* end if */
            printf("POOL UUID = %s\n", uuid_str);
        } /* end if */
        else {
            char uuid_buf[37];

            memcpy(pool_uuid, pool_uuid_g, sizeof(uuid_t));
            uuid_unparse(pool_uuid, uuid_buf);
            printf("POOL UUID = %s\n", uuid_buf);
        } /* end else */

        svcl_str = getenv ("DAOS_SVCL");
        if (svcl_str != NULL) {
            /* DSINC - this function creates an unavoidable memory leak, as the function
             * to free the memory it allocates is not currently exposed for use.
             */
            svcl = daos_rank_list_parse(svcl_str, ":");
            if (svcl == NULL) {
                fprintf(stderr, "Failed to parse SVC list env\n");
                return -1;
            }
        }
        printf("SVC LIST = %s\n", svcl_str);

        /* Connect to the pool */
        if(0 != (ret = daos_pool_connect(pool_uuid, pool_grp_g, svcl, DAOS_PC_RW, &H5_daos_poh_g, &pool_info, NULL /*event*/)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't connect to pool: %d", ret)

        /* Bcast pool handle if there are other processes */
        if(pool_num_procs > 1) {
            /* Calculate size of global pool handle */
            glob.iov_buf = NULL;
            glob.iov_buf_len = 0;
            glob.iov_len = 0;
            if(0 != (ret = daos_pool_local2global(H5_daos_poh_g, &glob)))
                D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't get global pool handle size: %d", ret)
            gh_buf_size = (uint64_t)glob.iov_buf_len;

            /* Check if the global handle won't fit into the static buffer */
            assert(sizeof(gh_buf_static) >= sizeof(uint64_t));
            if(gh_buf_size + sizeof(uint64_t) > sizeof(gh_buf_static)) {
                /* Allocate dynamic buffer */
                if(NULL == (gh_buf_dyn = (char *)DV_malloc(gh_buf_size + sizeof(uint64_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate space for global pool handle")

                /* Use dynamic buffer */
                gh_buf = gh_buf_dyn;
            } /* end if */

            /* Encode handle length */
            p = (uint8_t *)gh_buf;
            UINT64ENCODE(p, gh_buf_size)

            /* Get global pool handle */
            glob.iov_buf = (char *)p;
            glob.iov_buf_len = gh_buf_size;
            glob.iov_len = 0;
            if(0 != (ret = daos_pool_local2global(H5_daos_poh_g, &glob)))
                D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "can't get global pool handle: %d", ret)
            assert(glob.iov_len == glob.iov_buf_len);

            /* We are about to bcast so we no longer need to bcast on failure */
            must_bcast = FALSE;

            /* MPI_Bcast gh_buf */
            if(MPI_SUCCESS != MPI_Bcast(gh_buf, (int)sizeof(gh_buf_static), MPI_BYTE, 0, pool_comm_g))
                D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't bcast global pool handle")

            /* Need a second bcast if we had to allocate a dynamic buffer */
            if(gh_buf == gh_buf_dyn)
                if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)gh_buf_size, MPI_BYTE, 0, pool_comm_g))
                    D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't bcast global pool handle (second bcast)")
        } /* end if */
    } /* end if */
    else {
        /* Receive global handle */
        if(MPI_SUCCESS != MPI_Bcast(gh_buf, (int)sizeof(gh_buf_static), MPI_BYTE, 0, pool_comm_g))
            D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't bcast global pool handle")

        /* Decode handle length */
        p = (uint8_t *)gh_buf;
        UINT64DECODE(p, gh_buf_size)

        /* Check for gh_buf_size set to 0 - indicates failure */
        if(gh_buf_size == 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CANTINIT, FAIL, "lead process failed to initialize")

        /* Check if we need to perform another bcast */
        if(gh_buf_size + sizeof(uint64_t) > sizeof(gh_buf_static)) {
            /* Check if we need to allocate a dynamic buffer */
            if(gh_buf_size > sizeof(gh_buf_static)) {
                /* Allocate dynamic buffer */
                if(NULL == (gh_buf_dyn = (char *)DV_malloc(gh_buf_size)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate space for global pool handle")
                gh_buf = gh_buf_dyn;
            } /* end if */

            /* Receive global handle */
            if(MPI_SUCCESS != MPI_Bcast(gh_buf, (int)gh_buf_size, MPI_BYTE, 0, pool_comm_g))
                D_GOTO_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't bcast global pool handle (second bcast)")

            p = (uint8_t *)gh_buf;
        } /* end if */

        /* Create local pool handle */
        glob.iov_buf = (char *)p;
        glob.iov_buf_len = gh_buf_size;
        glob.iov_len = gh_buf_size;
        if(0 != (ret = daos_pool_global2local(glob, &H5_daos_poh_g)))
            D_GOTO_ERROR(H5E_VOL, H5E_CANTOPENOBJ, FAIL, "can't get local pool handle: %d", ret)
    } /* end else */

done:
    if(ret_value < 0) {
        /* Bcast gh_buf as '0' if necessary - this will trigger failures in the
         * other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(gh_buf_static, 0, sizeof(gh_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(gh_buf_static, sizeof(gh_buf_static), MPI_BYTE, 0, pool_comm_g))
                D_DONE_ERROR(H5E_VOL, H5E_MPI, FAIL, "can't Bcast empty global handle")
        } /* end if */

        H5daos_term();
    } /* end if */

    DV_free(gh_buf_dyn);

    D_FUNC_LEAVE
} /* end H5_daos_init() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_term
 *
 * Purpose:     Shut down the DAOS VOL
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              March, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5daos_term(void)
{
    herr_t ret_value = SUCCEED;            /* Return value */

    /* H5TRACE0("e",""); DSINC */

    /* Terminate the plugin */
    if(H5_daos_term() < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't close DAOS VOL plugin")

done:
#ifdef DV_TRACK_MEM_USAGE
    /* Check for allocated memory */
    if (0 != daos_vol_curr_alloc_bytes)
        FUNC_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "%zu bytes were still left allocated", daos_vol_curr_alloc_bytes)

    daos_vol_curr_alloc_bytes = 0;
#endif

    /* Unregister from the HDF5 error API */
    if (dv_err_class_g >= 0) {
        if (H5Eunregister_class(dv_err_class_g) < 0)
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't unregister from HDF5 error API")

        /* Print the current error stack before destroying it */
        PRINT_ERROR_STACK

        /* Destroy the error stack */
        if (H5Eclose_stack(dv_err_stack_g) < 0) {
            D_DONE_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't close error stack")
            PRINT_ERROR_STACK
        } /* end if */

        dv_err_stack_g = -1;
        dv_err_class_g = -1;
    } /* end if */

    D_FUNC_LEAVE_API
} /* end H5daos_term() */


/*---------------------------------------------------------------------------
 * Function:    H5_daos_term
 *
 * Purpose:     Shut down the DAOS VOL
 *
 * Returns:     Non-negative on success/Negative on failure
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5_daos_term(void)
{
    int ret;
    herr_t ret_value = SUCCEED;

    if(H5_DAOS_g >= 0) {
        /* Disconnect from pool */
        if(!daos_handle_is_inval(H5_daos_poh_g)) {
            if(0 != (ret = daos_pool_disconnect(H5_daos_poh_g, NULL /*event*/)))
                D_GOTO_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't disconnect from pool: %d", ret)
            H5_daos_poh_g = DAOS_HDL_INVAL;
        } /* end if */

        /* Terminate DAOS */
        if (daos_fini() < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "DAOS failed to terminate")

#ifdef DV_HAVE_SNAP_OPEN_ID
        /* Unregister the DAOS SNAP_OPEN_ID property from HDF5 */
        if (H5Punregister(H5P_FILE_ACCESS, H5_DAOS_SNAP_OPEN_ID) < 0)
            D_GOTO_ERROR(H5E_VOL, H5E_CLOSEERROR, FAIL, "can't unregister DAOS SNAP_OPEN_ID property")
#endif
    } /* end if */

done:
    /* "Forget" plugin id.  This should normally be called by the library
     * when it is closing the id, so no need to close it here. */
    H5_DAOS_g = -1;

    D_FUNC_LEAVE
} /* end H5_daos_term() */


/*-------------------------------------------------------------------------
 * Function:    H5Pset_fapl_daos
 *
 * Purpose:     Modify the file access property list to use the DAOS VOL
 *              plugin defined in this source file.  file_comm and
 *              file_info identify the communicator and info object used
 *              to coordinate actions on file create, open, flush, and
 *              close.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5Pset_fapl_daos(hid_t fapl_id, MPI_Comm file_comm, MPI_Info file_info)
{
    H5_daos_fapl_t fa;
    htri_t         is_fapl;
    herr_t         ret_value;

    /* H5TRACE3("e", "iMcMi", fapl_id, file_comm, file_info); DSINC */

    if(H5_DAOS_g < 0)
        D_GOTO_ERROR(H5E_VOL, H5E_UNINITIALIZED, FAIL, "DAOS VOL plugin not initialized")

    if(fapl_id == H5P_DEFAULT)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "can't set values in default property list")

    if((is_fapl = H5Pisa_class(fapl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class")
    if(!is_fapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list")

    if(MPI_COMM_NULL == file_comm)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADTYPE, FAIL, "not a valid communicator")

    /* Initialize driver specific properties */
    fa.comm = file_comm;
    fa.info = file_info;

    ret_value = H5Pset_vol(fapl_id, H5_DAOS_g, &fa);

done:
    D_FUNC_LEAVE_API
} /* end H5Pset_fapl_daos() */


/*-------------------------------------------------------------------------
 * Function:    H5daos_snap_create
 *
 * Purpose:     Creates a snapshot and returns the snapshot ID.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
#ifdef DSINC
herr_t
H5daos_snap_create(hid_t loc_id, H5_daos_snap_id_t *snap_id)
{
    H5_daos_item_t *item;
    H5_daos_file_t *file;
    H5VL_object_t     *obj = NULL;    /* object token of loc_id */
    herr_t          ret_value = SUCCEED;

    /* get the location object */
    if(NULL == (obj = (H5VL_object_t *)H5I_object(loc_id)))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "invalid location identifier")

    /* Make sure object's VOL is this one */
    if(obj->driver->id != H5_DAOS_g)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "location does not use DAOS VOL plugin")

    /* Get file object */
    if (NULL == (item = H5VLobject(loc_id)))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a VOL object")

    file = item->file;

    /* Check for write access */
    if(!(file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "no write intent on file")

    /* Tell the file to save a snapshot next time it is flushed (committed) */
    file->snap_epoch = (int)TRUE;

    /* Return epoch in snap_id */
    *snap_id = (uint64_t)file->epoch;

done:
    D_FUNC_LEAVE_API
} /* end H5daos_snap_create() */
#endif


/*-------------------------------------------------------------------------
 * Function:    H5Pset_daos_snap_open
 *
 * XXX: text to be changed
 * Purpose:     Modify the file access property list to use the DAOS VOL
 *              plugin defined in this source file.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
#ifdef DV_HAVE_SNAP_OPEN_ID
herr_t
H5Pset_daos_snap_open(hid_t fapl_id, H5_daos_snap_id_t snap_id)
{
    htri_t is_fapl;
    herr_t ret_value = SUCCEED;

    if(fapl_id == H5P_DEFAULT)
        D_GOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "can't set values in default property list")

    if((is_fapl = H5Pisa_class(fapl_id, H5P_FILE_ACCESS)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "couldn't determine property list class")
    if(!is_fapl)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list")

    /* Set the property */
    if(H5Pset(fapl_id, H5_DAOS_SNAP_OPEN_ID, &snap_id) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTSET, FAIL, "can't set property value for snap id")

done:
    D_FUNC_LEAVE_API
} /* end H5Pset_daos_snap_open() */
#endif


/*-------------------------------------------------------------------------
 * Function:    H5_daos_fapl_copy
 *
 * Purpose:     Copies the DAOS-specific file access properties.
 *
 * Return:      Success:        Ptr to a new property list
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_fapl_copy(const void *_old_fa)
{
    const H5_daos_fapl_t *old_fa = (const H5_daos_fapl_t*)_old_fa;
    H5_daos_fapl_t       *new_fa = NULL;
    void                 *ret_value = NULL;

    if(NULL == (new_fa = (H5_daos_fapl_t *)DV_malloc(sizeof(H5_daos_fapl_t))))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "memory allocation failed")

    /* Copy the general information */
    memcpy(new_fa, old_fa, sizeof(H5_daos_fapl_t));

    /* Clear allocated fields, so they aren't freed if something goes wrong.  No
     * need to clear info since it is only freed if comm is not null. */
    new_fa->comm = MPI_COMM_NULL;

    /* Duplicate communicator and Info object. */
    if(FAIL == H5FDmpi_comm_info_dup(old_fa->comm, old_fa->info, &new_fa->comm, &new_fa->info))
        D_GOTO_ERROR(H5E_INTERNAL, H5E_CANTCOPY, NULL, "Communicator/Info duplicate failed")

    ret_value = new_fa;

done:
    if (NULL == ret_value) {
        /* cleanup */
        if(new_fa && H5_daos_fapl_free(new_fa) < 0)
            D_DONE_ERROR(H5E_PLIST, H5E_CANTFREE, NULL, "can't free fapl")
    } /* end if */

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_fapl_copy() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_fapl_free
 *
 * Purpose:     Frees the DAOS-specific file access properties.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_fapl_free(void *_fa)
{
    H5_daos_fapl_t *fa = (H5_daos_fapl_t*) _fa;
    herr_t          ret_value = SUCCEED;

    assert(fa);

    /* Free the internal communicator and INFO object */
    if(fa->comm != MPI_COMM_NULL)
        if(H5FDmpi_comm_info_free(&fa->comm, &fa->info) < 0)
            D_GOTO_ERROR(H5E_INTERNAL, H5E_CANTFREE, FAIL, "Communicator/Info free failed")

    /* free the struct */
    DV_free(fa);

done:
    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_fapl_free() */


/* Create a DAOS OID given the object type and a 64 bit address (with the object
 * type already encoded) */
static void
H5_daos_oid_generate(daos_obj_id_t *oid, uint64_t addr, H5I_type_t obj_type)
{
    /* Encode type and address */
    oid->lo = addr;

    /* Generate oid */
    daos_obj_generate_id(oid, DAOS_OF_DKEY_HASHED | DAOS_OF_AKEY_HASHED,
            obj_type == H5I_DATASET ? DAOS_OC_LARGE_RW : DAOS_OC_TINY_RW);

    return;
} /* end H5_daos_oid_generate() */


/* Create a DAOS OID given the object type and a 64 bit index (top 2 bits are
 * ignored) */
static void
H5_daos_oid_encode(daos_obj_id_t *oid, uint64_t idx, H5I_type_t obj_type)
{
    uint64_t type_bits;

    /* Set type_bits */
    if(obj_type == H5I_GROUP)
        type_bits = H5_DAOS_TYPE_GRP;
    else if(obj_type == H5I_DATASET)
        type_bits = H5_DAOS_TYPE_DSET;
    else if(obj_type == H5I_DATATYPE)
        type_bits = H5_DAOS_TYPE_DTYPE;
    else {
#ifdef DV_HAVE_MAP
        assert(obj_type == H5I_MAP);
#endif
        type_bits = H5_DAOS_TYPE_MAP;
    } /* end else */

    /* Encode type and address and generate oid */
    H5_daos_oid_generate(oid, type_bits | (idx & H5_DAOS_IDX_MASK), obj_type);

    return;
} /* end H5_daos_oid_encode() */


/* Retrieve the 64 bit address from a DAOS OID */
static H5I_type_t
H5_daos_addr_to_type(uint64_t addr)
{
    uint64_t type_bits;

    /* Retrieve type */
    type_bits = addr & H5_DAOS_TYPE_MASK;
    if(type_bits == H5_DAOS_TYPE_GRP)
        return(H5I_GROUP);
    else if(type_bits == H5_DAOS_TYPE_DSET)
        return(H5I_DATASET);
    else if(type_bits == H5_DAOS_TYPE_DTYPE)
        return(H5I_DATATYPE);
#ifdef DV_HAVE_MAP
    else if(type_bits == H5_DAOS_TYPE_MAP)
        return(H5I_MAP);
#endif
    else
        return(H5I_BADID);
} /* end H5_daos_addr_to_type() */


/* Retrieve the 64 bit address from a DAOS OID */
static H5I_type_t
H5_daos_oid_to_type(daos_obj_id_t oid)
{
    /* Retrieve type */
    return H5_daos_addr_to_type(oid.lo);
} /* end H5_daos_oid_to_type() */


/* Retrieve the 64 bit object index from a DAOS OID */
static uint64_t
H5_daos_oid_to_idx(daos_obj_id_t oid)
{
    return oid.lo & H5_DAOS_IDX_MASK;
} /* end H5_daos_oid_to_idx() */


/* Multiply two 128 bit unsigned integers to yield a 128 bit unsigned integer */
static void
H5_daos_mult128(uint64_t x_lo, uint64_t x_hi, uint64_t y_lo, uint64_t y_hi,
    uint64_t *ans_lo, uint64_t *ans_hi)
{
    uint64_t xlyl;
    uint64_t xlyh;
    uint64_t xhyl;
    uint64_t xhyh;
    uint64_t temp;

    /*
     * First calculate x_lo * y_lo
     */
    /* Compute 64 bit results of multiplication of each combination of high and
     * low 32 bit sections of x_lo and y_lo */
    xlyl = (x_lo & 0xffffffff) * (y_lo & 0xffffffff);
    xlyh = (x_lo & 0xffffffff) * (y_lo >> 32);
    xhyl = (x_lo >> 32) * (y_lo & 0xffffffff);
    xhyh = (x_lo >> 32) * (y_lo >> 32);

    /* Calculate lower 32 bits of the answer */
    *ans_lo = xlyl & 0xffffffff;

    /* Calculate second 32 bits of the answer. Use temp to keep a 64 bit result
     * of the calculation for these 32 bits, to keep track of overflow past
     * these 32 bits. */
    temp = (xlyl >> 32) + (xlyh & 0xffffffff) + (xhyl & 0xffffffff);
    *ans_lo += temp << 32;

    /* Calculate third 32 bits of the answer, including overflowed result from
     * the previous operation */
    temp >>= 32;
    temp += (xlyh >> 32) + (xhyl >> 32) + (xhyh & 0xffffffff);
    *ans_hi = temp & 0xffffffff;

    /* Calculate highest 32 bits of the answer. No need to keep track of
     * overflow because it has overflowed past the end of the 128 bit answer */
    temp >>= 32;
    temp += (xhyh >> 32);
    *ans_hi += temp << 32;

    /*
     * Now add the results from multiplying x_lo * y_hi and x_hi * y_lo. No need
     * to consider overflow here, and no need to consider x_hi * y_hi because
     * those results would overflow past the end of the 128 bit answer.
     */
    *ans_hi += (x_lo * y_hi) + (x_hi * y_lo);

    return;
} /* end H5_daos_mult128() */


/* Implementation of the FNV hash algorithm */
static void
H5_daos_hash128(const char *name, void *hash)
{
    const uint8_t *name_p = (const uint8_t *)name;
    uint8_t *hash_p = (uint8_t *)hash;
    uint64_t name_lo;
    uint64_t name_hi;
    /* Initialize hash value in accordance with the FNV algorithm */
    uint64_t hash_lo = 0x62b821756295c58d;
    uint64_t hash_hi = 0x6c62272e07bb0142;
    /* Initialize FNV prime number in accordance with the FNV algorithm */
    const uint64_t fnv_prime_lo = 0x13b;
    const uint64_t fnv_prime_hi = 0x1000000;
    size_t name_len_rem = strlen(name);

    while(name_len_rem > 0) {
        /* "Decode" lower 64 bits of this 128 bit section of the name, so the
         * numberical value of the integer is the same on both little endian and
         * big endian systems */
        if(name_len_rem >= 8) {
            UINT64DECODE(name_p, name_lo)
            name_len_rem -= 8;
        } /* end if */
        else {
            name_lo = 0;
            UINT64DECODE_VAR(name_p, name_lo, name_len_rem)
            name_len_rem = 0;
        } /* end else */

        /* "Decode" second 64 bits */
        if(name_len_rem > 0) {
            if(name_len_rem >= 8) {
                UINT64DECODE(name_p, name_hi)
                name_len_rem -= 8;
            } /* end if */
            else {
                name_hi = 0;
                UINT64DECODE_VAR(name_p, name_hi, name_len_rem)
                name_len_rem = 0;
            } /* end else */
        } /* end if */
        else
            name_hi = 0;

        /* FNV algorithm - XOR hash with name then multiply by fnv_prime */
        hash_lo ^= name_lo;
        hash_hi ^= name_hi;
        H5_daos_mult128(hash_lo, hash_hi, fnv_prime_lo, fnv_prime_hi, &hash_lo, &hash_hi);
    } /* end while */

    /* "Encode" hash integers to char buffer, so the buffer is the same on both
     * little endian and big endian systems */
    UINT64ENCODE(hash_p, hash_lo)
    UINT64ENCODE(hash_p, hash_hi)

    return;
} /* end H5_daos_hash128() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_file_create
 *
 * Purpose:     Creates a file as a daos HDF5 file.
 *
 * Return:      Success:        the file ID.
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              September, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_file_create(const char *name, unsigned flags, hid_t fcpl_id,
    hid_t fapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_fapl_t *fa = NULL;
    H5_daos_file_t *file = NULL;
    daos_iov_t glob;
    uint64_t gh_buf_size;
    char gh_buf_static[H5_DAOS_GH_BUF_SIZE];
    char *gh_buf_dyn = NULL;
    char *gh_buf = gh_buf_static;
    daos_obj_id_t gmd_oid = {0, 0};
    uint8_t *p;
    hbool_t must_bcast = FALSE;
    int ret;
    void *ret_value = NULL;

    /*
     * Adjust bit flags by turning on the creation bit and making sure that
     * the EXCL or TRUNC bit is set.  All newly-created files are opened for
     * reading and writing.
     */
    if(0==(flags & (H5F_ACC_EXCL|H5F_ACC_TRUNC)))
        flags |= H5F_ACC_EXCL;      /*default*/
    flags |= H5F_ACC_RDWR | H5F_ACC_CREAT;

    /* Get information from the FAPL */
    /*
     * XXX: DSINC - may no longer need to use this VOL info.
     */
    if(H5Pget_vol_info(fapl_id, (void **) &fa) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_CANTGET, NULL, "can't get DAOS info struct")

    /* allocate the file object that is returned to the user */
    if(NULL == (file = H5FL_CALLOC(H5_daos_file_t)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTALLOC, NULL, "can't allocate DAOS file struct")
    file->glob_md_oh = DAOS_HDL_INVAL;
    file->root_grp = NULL;
    file->fcpl_id = FAIL;
    file->fapl_id = FAIL;
    file->vol_id = FAIL;

    /* Fill in fields of file we know */
    file->item.type = H5I_FILE;
    file->item.file = file;
    file->item.rc = 1;
    if(NULL == (file->file_name = strdup(name)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't copy file name")
    file->flags = flags;
    file->max_oid = 0;
    file->max_oid_dirty = FALSE;
    if((file->fcpl_id = H5Pcopy(fcpl_id)) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_CANTCOPY, NULL, "failed to copy fcpl")
    if((file->fapl_id = H5Pcopy(fapl_id)) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_CANTCOPY, NULL, "failed to copy fapl")

    /* Duplicate communicator and Info object. */
    /*
     * XXX: DSINC - Need to pass in MPI Info to VOL connector as well.
     */
    if(FAIL == H5FDmpi_comm_info_dup(fa ? fa->comm : pool_comm_g, fa ? fa->info : MPI_INFO_NULL, &file->comm, &file->info))
        D_GOTO_ERROR(H5E_INTERNAL, H5E_CANTCOPY, NULL, "Communicator/Info duplicate failed")

    /* Obtain the process rank and size from the communicator attached to the
     * fapl ID */
    MPI_Comm_rank(fa ? fa->comm : pool_comm_g, &file->my_rank);
    MPI_Comm_size(fa ? fa->comm : pool_comm_g, &file->num_procs);

    /* Hash file name to create uuid */
    H5_daos_hash128(name, &file->uuid);

    /* Determine if we requested collective object ops for the file */
    if(H5Pget_all_coll_metadata_ops(fapl_id, &file->collective) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_CANTGET, NULL, "can't get collective access property")

    /* Generate oid for global metadata object */
    daos_obj_generate_id(&gmd_oid, DAOS_OF_DKEY_HASHED | DAOS_OF_AKEY_HASHED, DAOS_OC_TINY_RW);

    if(file->my_rank == 0) {
        /* If there are other processes and we fail we must bcast anyways so they
         * don't hang */
        if(file->num_procs > 1)
            must_bcast = TRUE;

        /* Delete the container if H5F_ACC_TRUNC is set.  This shouldn't cause a
         * problem even if the container doesn't exist. */
        /* Need to handle EXCL correctly DSINC */
        if(flags & H5F_ACC_TRUNC)
            if(0 != (ret = daos_cont_destroy(H5_daos_poh_g, file->uuid, 1, NULL /*event*/)))
                D_GOTO_ERROR(H5E_FILE, H5E_CANTCREATE, NULL, "can't destroy container: %d", ret)

        /* Create the container for the file */
        if(0 != (ret = daos_cont_create(H5_daos_poh_g, file->uuid, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTCREATE, NULL, "can't create container: %d", ret)

        /* Open the container */
        if(0 != (ret = daos_cont_open(H5_daos_poh_g, file->uuid, DAOS_COO_RW, &file->coh, NULL /*&file->co_info*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, NULL, "can't open container: %d", ret)

        /* Open global metadata object */
        if(0 != (ret = daos_obj_open(file->coh, gmd_oid, DAOS_OO_RW, &file->glob_md_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, NULL, "can't open global metadata object: %d", ret)

        /* Bcast global container handle if there are other processes */
        if(file->num_procs > 1) {
            /* Calculate size of the global container handle */
            glob.iov_buf = NULL;
            glob.iov_buf_len = 0;
            glob.iov_len = 0;
            if(0 != (ret = daos_cont_local2global(file->coh, &glob)))
                D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't get global container handle size: %d", ret)
            gh_buf_size = (uint64_t)glob.iov_buf_len;

            /* Check if the global handle won't fit into the static buffer */
            assert(sizeof(gh_buf_static) >= sizeof(uint64_t));
            if(gh_buf_size + sizeof(uint64_t) > sizeof(gh_buf_static)) {
                /* Allocate dynamic buffer */
                if(NULL == (gh_buf_dyn = (char *)DV_malloc(gh_buf_size + sizeof(uint64_t))))
                    D_GOTO_ERROR(H5E_FILE, H5E_CANTALLOC, NULL, "can't allocate space for global container handle")

                /* Use dynamic buffer */
                gh_buf = gh_buf_dyn;
            } /* end if */

            /* Encode handle length */
            p = (uint8_t *)gh_buf;
            UINT64ENCODE(p, gh_buf_size)

            /* Retrieve global container handle */
            glob.iov_buf = (char *)p;
            glob.iov_buf_len = gh_buf_size;
            glob.iov_len = 0;
            if(0 != (ret = daos_cont_local2global(file->coh, &glob)))
                D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't get global container handle: %d", ret)
            assert(glob.iov_len == glob.iov_buf_len);

            /* We are about to bcast so we no longer need to bcast on failure */
            must_bcast = FALSE;

            /* MPI_Bcast gh_buf */
            if(MPI_SUCCESS != MPI_Bcast(gh_buf, (int)sizeof(gh_buf_static), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global container handle")

            /* Need a second bcast if we had to allocate a dynamic buffer */
            if(gh_buf == gh_buf_dyn)
                if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)gh_buf_size, MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                    D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global container handle (second bcast)")
        } /* end if */
    } /* end if */
    else {
        /* Receive global handle */
        if(MPI_SUCCESS != MPI_Bcast(gh_buf, (int)sizeof(gh_buf_static), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
            D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global container handle")

        /* Decode handle length */
        p = (uint8_t *)gh_buf;
        UINT64DECODE(p, gh_buf_size)

        /* Check for gh_buf_size set to 0 - indicates failure */
        if(gh_buf_size == 0)
            D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "lead process failed to open file")

        /* Check if we need to perform another bcast */
        if(gh_buf_size + sizeof(uint64_t) > sizeof(gh_buf_static)) {
            /* Check if we need to allocate a dynamic buffer */
            if(gh_buf_size > sizeof(gh_buf_static)) {
                /* Allocate dynamic buffer */
                if(NULL == (gh_buf_dyn = (char *)DV_malloc(gh_buf_size)))
                    D_GOTO_ERROR(H5E_FILE, H5E_CANTALLOC, NULL, "can't allocate space for global pool handle")
                gh_buf = gh_buf_dyn;
            } /* end if */

            /* Receive global handle */
            if(MPI_SUCCESS != MPI_Bcast(gh_buf_dyn, (int)gh_buf_size, MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global container handle (second bcast)")

            p = (uint8_t *)gh_buf;
        } /* end if */

        /* Create local container handle */
        glob.iov_buf = (char *)p;
        glob.iov_buf_len = gh_buf_size;
        glob.iov_len = gh_buf_size;
        if(0 != (ret = daos_cont_global2local(H5_daos_poh_g, glob, &file->coh)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENOBJ, NULL, "can't get local container handle: %d", ret)

        /* Open global metadata object */
        if(0 != (ret = daos_obj_open(file->coh, gmd_oid, DAOS_OO_RW, &file->glob_md_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, NULL, "can't open global metadata object: %d", ret)
    } /* end else */
 
    /* Create root group */
    if(NULL == (file->root_grp = (H5_daos_group_t *)H5_daos_group_create_helper(file, fcpl_id, H5P_GROUP_ACCESS_DEFAULT, dxpl_id, req, NULL, NULL, 0, TRUE)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't create root group")
    assert(file->root_grp->obj.oid.lo == (uint64_t)1);

    ret_value = (void *)file;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast bcast_buf_64 as '0' if necessary - this will trigger failures
         * in the other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(gh_buf_static, 0, sizeof(gh_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(gh_buf_static, sizeof(gh_buf_static), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                D_DONE_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global handle sizes")
        } /* end if */

        /* Close file */
        if(file && H5_daos_file_close_helper(file, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, NULL, "can't close file")
    } /* end if */

    if(fa)
        H5VLfree_connector_info(H5_DAOS_g, fa);

    /* Clean up */
    DV_free(gh_buf_dyn);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_file_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_file_open
 *
 * Purpose:     Opens a file as a daos HDF5 file.
 *
 * Return:      Success:        the file ID.
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_file_open(const char *name, unsigned flags, hid_t fapl_id,
    hid_t dxpl_id, void **req)
{
    H5_daos_fapl_t *fa = NULL;
    H5_daos_file_t *file = NULL;
#ifdef DV_HAVE_SNAP_OPEN_ID
    H5_daos_snap_id_t snap_id;
#endif
    daos_iov_t glob;
    uint64_t gh_len;
    char foi_buf_static[H5_DAOS_FOI_BUF_SIZE];
    char *foi_buf_dyn = NULL;
    char *foi_buf = foi_buf_static;
    void *gcpl_buf = NULL;
    uint64_t gcpl_len;
    daos_obj_id_t gmd_oid = {0, 0};
    daos_obj_id_t root_grp_oid = {0, 0};
    uint8_t *p;
    hbool_t must_bcast = FALSE;
    int ret;
    void *ret_value = NULL;

    /* Get information from the FAPL */
    /*
     * XXX: DSINC - may no longer need to use this VOL info.
     */
    if(H5Pget_vol_info(fapl_id, (void **) &fa) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, NULL, "can't get DAOS info struct")

#ifdef DV_HAVE_SNAP_OPEN_ID
    if(H5Pget(fapl_id, H5_DAOS_SNAP_OPEN_ID, &snap_id) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, NULL, "can't get property value for snap id")

    /* Check for opening a snapshot with write access (disallowed) */
    if((snap_id != H5_DAOS_SNAP_ID_INVAL) && (flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "write access requested to snapshot - disallowed")
#endif

    /* allocate the file object that is returned to the user */
    if(NULL == (file = H5FL_CALLOC(H5_daos_file_t)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTALLOC, NULL, "can't allocate DAOS file struct")
    file->glob_md_oh = DAOS_HDL_INVAL;
    file->root_grp = NULL;
    file->fcpl_id = FAIL;
    file->fapl_id = FAIL;
    file->vol_id = FAIL;

    /* Fill in fields of file we know */
    file->item.type = H5I_FILE;
    file->item.file = file;
    file->item.rc = 1;
    if(NULL == (file->file_name = strdup(name)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't copy file name")
    file->flags = flags;
    if((file->fapl_id = H5Pcopy(fapl_id)) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_CANTCOPY, NULL, "failed to copy fapl")

    /* Duplicate communicator and Info object. */
    /*
     * XXX: DSINC - Need to pass in MPI Info to VOL connector as well.
     */
    if(FAIL == H5FDmpi_comm_info_dup(fa ? fa->comm : pool_comm_g, fa ? fa->info : MPI_INFO_NULL, &file->comm, &file->info))
        D_GOTO_ERROR(H5E_INTERNAL, H5E_CANTCOPY, NULL, "Communicator/Info duplicate failed")

    /* Obtain the process rank and size from the communicator attached to the
     * fapl ID */
    MPI_Comm_rank(fa ? fa->comm : pool_comm_g, &file->my_rank);
    MPI_Comm_size(fa ? fa->comm : pool_comm_g, &file->num_procs);

    /* Hash file name to create uuid */
    H5_daos_hash128(name, &file->uuid);

    /* Generate oid for global metadata object */
    daos_obj_generate_id(&gmd_oid, DAOS_OF_DKEY_HASHED | DAOS_OF_AKEY_HASHED, DAOS_OC_TINY_RW);

    /* Generate root group oid */
    H5_daos_oid_encode(&root_grp_oid, (uint64_t)1, H5I_GROUP);

    /* Determine if we requested collective object ops for the file */
    if(H5Pget_all_coll_metadata_ops(fapl_id, &file->collective) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_CANTGET, NULL, "can't get collective access property")

    if(file->my_rank == 0) {
        daos_key_t dkey;
        daos_iod_t iod;
        daos_sg_list_t sgl;
        daos_iov_t sg_iov;
        char int_md_key[] = H5_DAOS_INT_MD_KEY;
        char max_oid_key[] = H5_DAOS_MAX_OID_KEY;

        /* If there are other processes and we fail we must bcast anyways so they
         * don't hang */
        if(file->num_procs > 1)
            must_bcast = TRUE;

        /* Open the container */
        if(0 != (ret = daos_cont_open(H5_daos_poh_g, file->uuid, flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &file->coh, NULL /*&file->co_info*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, NULL, "can't open container: %d", ret)

        /* If a snapshot was requested, use it as the epoch, otherwise query it
         */
#ifdef DV_HAVE_SNAP_OPEN_ID
        if(snap_id != H5_DAOS_SNAP_ID_INVAL) {
            epoch = (daos_epoch_t)snap_id;

            assert(!(flags & H5F_ACC_RDWR));
        } /* end if */
        else {
#endif
#ifdef DV_HAVE_SNAP_OPEN_ID
        } /* end else */
#endif

        /* Open global metadata object */
        if(0 != (ret = daos_obj_open(file->coh, gmd_oid, flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &file->glob_md_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, NULL, "can't open global metadata object: %d", ret)

        /* Read max OID from gmd obj */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)max_oid_key, (daos_size_t)(sizeof(max_oid_key) - 1));
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_nr = 1u;
        iod.iod_size = (uint64_t)8;
        iod.iod_type = DAOS_IOD_SINGLE;

        /* Set up sgl */
        daos_iov_set(&sg_iov, &file->max_oid, (daos_size_t)8);
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &sg_iov;

        /* Read max OID from gmd obj */
        if(0 != (ret = daos_obj_fetch(file->glob_md_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTDECODE, NULL, "can't read max OID from global metadata object: %d", ret)

        /* Open root group */
        if(NULL == (file->root_grp = (H5_daos_group_t *)H5_daos_group_open_helper(file, root_grp_oid, H5P_GROUP_ACCESS_DEFAULT, dxpl_id, req, (file->num_procs > 1) ? &gcpl_buf : NULL, &gcpl_len)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't open root group")

        /* Bcast global handles if there are other processes */
        if(file->num_procs > 1) {
            /* Calculate size of the global container handle */
            glob.iov_buf = NULL;
            glob.iov_buf_len = 0;
            glob.iov_len = 0;
            if(0 != (ret = daos_cont_local2global(file->coh, &glob)))
                D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't get global container handle size: %d", ret)
            gh_len = (uint64_t)glob.iov_buf_len;

            /* Check if the file open info won't fit into the static buffer */
            if(gh_len + gcpl_len + 3 * sizeof(uint64_t) > sizeof(foi_buf_static)) {
                /* Allocate dynamic buffer */
                if(NULL == (foi_buf_dyn = (char *)DV_malloc(gh_len + gcpl_len + 3 * sizeof(uint64_t))))
                    D_GOTO_ERROR(H5E_FILE, H5E_CANTALLOC, NULL, "can't allocate space for global container handle")

                /* Use dynamic buffer */
                foi_buf = foi_buf_dyn;
            } /* end if */

            /* Encode handle length */
            p = (uint8_t *)foi_buf;
            UINT64ENCODE(p, gh_len)

            /* Encode GCPL length */
            UINT64ENCODE(p, gcpl_len)

            /* Encode max OID */
            UINT64ENCODE(p, file->max_oid)

            /* Retrieve global container handle */
            glob.iov_buf = (char *)p;
            glob.iov_buf_len = gh_len;
            glob.iov_len = 0;
            if(0 != (ret = daos_cont_local2global(file->coh, &glob)))
                D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't get file open info: %d", ret)
            assert(glob.iov_len == glob.iov_buf_len);

            /* Copy GCPL buffer */
            memcpy(p + gh_len, gcpl_buf, gcpl_len);

            /* We are about to bcast so we no longer need to bcast on failure */
            must_bcast = FALSE;

            /* MPI_Bcast foi_buf */
            if(MPI_SUCCESS != MPI_Bcast(foi_buf, (int)sizeof(foi_buf_static), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global container handle")

            /* Need a second bcast if we had to allocate a dynamic buffer */
            if(foi_buf == foi_buf_dyn)
                if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)(gh_len + gcpl_len), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                    D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast file open info (second bcast)")
        } /* end if */
    } /* end if */
    else {
        /* Receive file open info */
        if(MPI_SUCCESS != MPI_Bcast(foi_buf, (int)sizeof(foi_buf_static), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
            D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global container handle")

        /* Decode handle length */
        p = (uint8_t *)foi_buf;
        UINT64DECODE(p, gh_len)

        /* Check for gh_len set to 0 - indicates failure */
        if(gh_len == 0)
            D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "lead process failed to open file")

        /* Decode GCPL length */
        UINT64DECODE(p, gcpl_len)

        /* Decode max OID */
        UINT64DECODE(p, file->max_oid)

        /* Check if we need to perform another bcast */
        if(gh_len + gcpl_len + 3 * sizeof(uint64_t) > sizeof(foi_buf_static)) {
            /* Check if we need to allocate a dynamic buffer */
            if(gh_len + gcpl_len > sizeof(foi_buf_static)) {
                /* Allocate dynamic buffer */
                if(NULL == (foi_buf_dyn = (char *)DV_malloc(gh_len + gcpl_len)))
                    D_GOTO_ERROR(H5E_FILE, H5E_CANTALLOC, NULL, "can't allocate space for global pool handle")
                foi_buf = foi_buf_dyn;
            } /* end if */

            /* Receive global handle */
            if(MPI_SUCCESS != MPI_Bcast(foi_buf_dyn, (int)(gh_len + gcpl_len), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                D_GOTO_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global container handle (second bcast)")

            p = (uint8_t *)foi_buf;
        } /* end if */

        /* Create local container handle */
        glob.iov_buf = (char *)p;
        glob.iov_buf_len = gh_len;
        glob.iov_len = gh_len;
        if(0 != (ret = daos_cont_global2local(H5_daos_poh_g, glob, &file->coh)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENOBJ, NULL, "can't get local container handle: %d", ret)

        /* Open global metadata object */
        if(0 != (ret = daos_obj_open(file->coh, gmd_oid, flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &file->glob_md_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, NULL, "can't open global metadata object: %d", ret)

        /* Reconstitute root group from revieved GCPL */
        if(NULL == (file->root_grp = (H5_daos_group_t *)H5_daos_group_reconstitute(file, root_grp_oid, p + gh_len, H5P_GROUP_ACCESS_DEFAULT, dxpl_id, req)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't reconstitute root group")
    } /* end else */

    /* FCPL was stored as root group's GCPL (as GCPL is the parent of FCPL).
     * Point to it. */
    file->fcpl_id = file->root_grp->gcpl_id;
    if(H5Iinc_ref(file->fcpl_id) < 0)
        D_GOTO_ERROR(H5E_ATOM, H5E_CANTINC, NULL, "can't increment FCPL ref count")

    ret_value = (void *)file;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast bcast_buf_64 as '0' if necessary - this will trigger failures
         * in the other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(foi_buf_static, 0, sizeof(foi_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(foi_buf_static, sizeof(foi_buf_static), MPI_BYTE, 0, fa ? fa->comm : pool_comm_g))
                D_DONE_ERROR(H5E_FILE, H5E_MPI, NULL, "can't bcast global handle sizes")
        } /* end if */

        /* Close file */
        if(file && H5_daos_file_close_helper(file, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, NULL, "can't close file")
    } /* end if */

    if(fa)
        H5VLfree_connector_info(H5_DAOS_g, fa);

    /* Clean up buffers */
    foi_buf_dyn = (char *)DV_free(foi_buf_dyn);
    gcpl_buf = DV_free(gcpl_buf);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_file_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_file_flush
 *
 * Purpose:     Flushes a DAOS file.  Currently a no-op, may create a
 *              snapshot in the future.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_file_flush(H5_daos_file_t *file)
{
    int ret;
    herr_t       ret_value = SUCCEED;    /* Return value */

    /* Nothing to do if no write intent */
    if(!(file->flags & H5F_ACC_RDWR))
        D_GOTO_DONE(SUCCEED)

#if 0
    /* Collectively determine if anyone requested a snapshot of the epoch */
    if(MPI_SUCCESS != MPI_Reduce(file->my_rank == 0 ? MPI_IN_PLACE : &file->snap_epoch, &file->snap_epoch, 1, MPI_INT, MPI_LOR, 0, file->comm))
        D_GOTO_ERROR(H5E_FILE, H5E_MPI, FAIL, "failed to determine whether to take snapshot (MPI_Reduce)")

    /* Barrier on all ranks so we don't commit before all ranks are
     * finished writing. H5Fflush must be called collectively. */
    if(MPI_SUCCESS != MPI_Barrier(file->comm))
        D_GOTO_ERROR(H5E_FILE, H5E_MPI, FAIL, "MPI_Barrier failed")

    /* Commit the epoch */
    if(file->my_rank == 0) {
        /* Save a snapshot of this epoch if requested */
        /* Disabled until snapshots are supported in DAOS DSINC */

        if(file->snap_epoch)
            if(0 != (ret = daos_snap_create(file->coh, file->epoch, NULL /*event*/)))
                D_GOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, "can't create snapshot: %d", ret)

        /* Commit the epoch.  This should slip previous epochs automatically. */
        if(0 != (ret = daos_epoch_commit(file->coh, file->epoch, NULL /*state*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, FAIL, "failed to commit epoch: %d", ret)
    } /* end if */
#endif

done:
    D_FUNC_LEAVE
} /* end H5_daos_file_flush() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_file_specific
 *
 * Purpose:     Perform an operation
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_file_specific(void *item, H5VL_file_specific_t specific_type,
    hid_t DV_ATTR_UNUSED dxpl_id, void DV_ATTR_UNUSED **req,
    va_list DV_ATTR_UNUSED arguments)
{
    H5_daos_file_t *file = NULL;
    herr_t          ret_value = SUCCEED;    /* Return value */

    if (item)
        file = ((H5_daos_item_t *)item)->file;

    switch (specific_type) {
        /* H5Fflush */
        case H5VL_FILE_FLUSH:
            if(H5_daos_file_flush(file) < 0)
                D_GOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, "can't flush file")

            break;
        /* H5Fmount */
        case H5VL_FILE_MOUNT:
        /* H5Fmount */
        case H5VL_FILE_UNMOUNT:
        /* H5Fis_accessible */
        case H5VL_FILE_IS_ACCESSIBLE:
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "invalid or unsupported specific operation")
    } /* end switch */

done:
    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_file_specific() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_file_close_helper
 *
 * Purpose:     Closes a daos HDF5 file.
 *
 * Return:      Success:        the file id. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_file_close_helper(H5_daos_file_t *file, hid_t dxpl_id, void **req)
{
    int ret;
    herr_t ret_value = SUCCEED;

    assert(file);

    /* Free file data structures */
    if(file->file_name)
        free(file->file_name);
    if(file->comm || file->info)
        if(H5FDmpi_comm_info_free(&file->comm, &file->info) < 0)
            D_DONE_ERROR(H5E_INTERNAL, H5E_CANTFREE, FAIL, "Communicator/Info free failed")
    if(file->fapl_id != FAIL && H5Idec_ref(file->fapl_id) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist")
    if(file->fcpl_id != FAIL && H5Idec_ref(file->fcpl_id) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist")
    if(!daos_handle_is_inval(file->glob_md_oh))
        if(0 != (ret = daos_obj_close(file->glob_md_oh, NULL /*event*/)))
            D_DONE_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, FAIL, "can't close global metadata object: %d", ret)
    if(file->root_grp)
        if(H5_daos_group_close(file->root_grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, FAIL, "can't close root group")
    if(!daos_handle_is_inval(file->coh))
        if(0 != (ret = daos_cont_close(file->coh, NULL /*event*/)))
            D_DONE_ERROR(H5E_FILE, H5E_CLOSEERROR, FAIL, "can't close container: %d", ret)
    if(file->vol_id >= 0) {
        if(H5VLfree_connector_info(file->vol_id, file->vol_info) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CANTFREE, FAIL, "can't free vol connector info")
        if(H5Idec_ref(file->vol_id) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CANTDEC, FAIL, "can't decrement vol connector id")
    } /* end if */
    file = H5FL_FREE(H5_daos_file_t, file);

    D_FUNC_LEAVE
} /* end H5_daos_file_close_helper() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_file_close
 *
 * Purpose:     Closes a daos HDF5 file, committing the epoch if
 *              appropriate.
 *
 * Return:      Success:        the file ID.
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              October, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_file_close(void *_file, hid_t dxpl_id, void **req)
{
    H5_daos_file_t *file = (H5_daos_file_t *)_file;
#if 0 /* DSINC */
    int ret;
#endif
    herr_t ret_value = SUCCEED;

    assert(file);

    /* Flush the file (barrier, commit epoch, slip epoch) *Update comment DSINC */
    if(H5_daos_file_flush(file) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, "can't flush file")

#if 0 /* DSINC */
    /* Flush the epoch */
    if(0 != (ret = daos_epoch_flush(file->coh, epoch, NULL /*state*/, NULL /*event*/)))
        D_DONE_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, "can't flush epoch: %d", ret)
#endif

    /* Close the file */
    if(H5_daos_file_close_helper(file, dxpl_id, req) < 0)
        D_GOTO_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, FAIL, "can't close file")

done:
    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_file_close() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_write_max_oid
 *
 * Purpose:     Writes the max OID (object index) to the global metadata
 *              object
 *
 * Return:      Success:        0
 *              Failure:        1
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_write_max_oid(H5_daos_file_t *file)
{
    daos_key_t dkey;
    daos_iod_t iod;
    daos_sg_list_t sgl;
    daos_iov_t sg_iov;
    char int_md_key[] = H5_DAOS_INT_MD_KEY;
    char max_oid_key[] = H5_DAOS_MAX_OID_KEY;
    int ret;
    herr_t ret_value = SUCCEED;

    /* Set up dkey */
    daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

    /* Set up iod */
    memset(&iod, 0, sizeof(iod));
    daos_iov_set(&iod.iod_name, (void *)max_oid_key, (daos_size_t)(sizeof(max_oid_key) - 1));
    daos_csum_set(&iod.iod_kcsum, NULL, 0);
    iod.iod_nr = 1u;
    iod.iod_size = (uint64_t)8;
    iod.iod_type = DAOS_IOD_SINGLE;

    /* Set up sgl */
    daos_iov_set(&sg_iov, &file->max_oid, (daos_size_t)8);
    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &sg_iov;

    /* Write max OID to gmd obj */
    if(0 != (ret = daos_obj_update(file->glob_md_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTENCODE, FAIL, "can't write max OID to global metadata object: %d", ret)
done:
    D_FUNC_LEAVE
} /* end H5_daos_write_max_oid() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_read
 *
 * Purpose:     Reads the specified link from the given group.  Note that
 *              if the returned link is a soft link, val->target.soft must
 *              eventually be freed.
 *
 * Return:      Success:        SUCCEED 
 *              Failure:        FAIL
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_read(H5_daos_group_t *grp, const char *name, size_t name_len,
    H5_daos_link_val_t *val)
{
    char const_link_key[] = H5_DAOS_LINK_KEY;
    daos_key_t dkey;
    daos_iod_t iod;
    daos_sg_list_t sgl;
    daos_iov_t sg_iov;
    uint8_t *val_buf;
    uint8_t val_buf_static[H5_DAOS_LINK_VAL_BUF_SIZE];
    uint8_t *val_buf_dyn = NULL;
    uint8_t *p;
    int ret;
    herr_t ret_value = SUCCEED;
 
    /* Use static link value buffer initially */
    val_buf = val_buf_static;

    /* Set up dkey */
    daos_iov_set(&dkey, (void *)name, (daos_size_t)name_len);

    /* Set up iod */
    memset(&iod, 0, sizeof(iod));
    daos_iov_set(&iod.iod_name, const_link_key, (daos_size_t)(sizeof(const_link_key) - 1));
    daos_csum_set(&iod.iod_kcsum, NULL, 0);
    iod.iod_nr = 1u;
    iod.iod_size = DAOS_REC_ANY;
    iod.iod_type = DAOS_IOD_SINGLE;

    /* Set up sgl */
    daos_iov_set(&sg_iov, val_buf, (daos_size_t)H5_DAOS_LINK_VAL_BUF_SIZE);
    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &sg_iov;

    /* Read link */
    if(0 != (ret = daos_obj_fetch(grp->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't read link: %d", ret)

    /* Check for no link found */
    if(iod.iod_size == (uint64_t)0)
        D_GOTO_ERROR(H5E_SYM, H5E_NOTFOUND, FAIL, "link not found")

    /* Check if val_buf was large enough */
    if(iod.iod_size > (uint64_t)H5_DAOS_LINK_VAL_BUF_SIZE) {
        /* Allocate new value buffer */
        if(NULL == (val_buf_dyn = (uint8_t *)DV_malloc(iod.iod_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate link value buffer")

        /* Point to new buffer */
        val_buf = val_buf_dyn;
        daos_iov_set(&sg_iov, val_buf, (daos_size_t)iod.iod_size);

        /* Reissue read */
        if(0 != (ret = daos_obj_fetch(grp->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps */, NULL /*event*/)))
            D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't read link: %d", ret)
    } /* end if */

    /* Decode link type */
    p = val_buf;
    val->type = (H5L_type_t)*p++;

    /* Decode remainder of link value */
    switch(val->type) {
        case H5L_TYPE_HARD:
            /* Decode oid */
            UINT64DECODE(p, val->target.hard.lo)
            UINT64DECODE(p, val->target.hard.hi)

            break;

        case H5L_TYPE_SOFT:
            /* If we had to allocate a buffer to read from daos, it happens to
             * be the exact size (len + 1) we need for the soft link value,
             * take ownership of it and shift the value down one byte.
             * Otherwise, allocate a new buffer. */
            if(val_buf_dyn) {
                val->target.soft = (char *)val_buf_dyn;
                val_buf_dyn = NULL;
                memmove(val->target.soft,  val->target.soft + 1, iod.iod_size - 1);
            } /* end if */
            else {
                if(NULL == (val->target.soft = (char *)DV_malloc(iod.iod_size)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate link value buffer")
                memcpy(val->target.soft, val_buf + 1, iod.iod_size - 1);
            } /* end else */

            /* Add null terminator */
            val->target.soft[iod.iod_size - 1] = '\0';

            break;

        case H5L_TYPE_ERROR:
        case H5L_TYPE_EXTERNAL:
        case H5L_TYPE_MAX:
        default:
            D_GOTO_ERROR(H5E_SYM, H5E_BADVALUE, FAIL, "invalid or unsupported link type")
    } /* end switch */

done:
    if(val_buf_dyn) {
        assert(ret_value == FAIL);
        DV_free(val_buf_dyn);
    } /* end if */

    D_FUNC_LEAVE
} /* end H5_daos_link_read() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_write
 *
 * Purpose:     Writes the specified link to the given group
 *
 * Return:      Success:        SUCCEED 
 *              Failure:        FAIL
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_write(H5_daos_group_t *grp, const char *name,
    size_t name_len, H5_daos_link_val_t *val)
{
    char const_link_key[] = H5_DAOS_LINK_KEY;
    daos_key_t dkey;
    daos_iod_t iod;
    daos_sg_list_t sgl;
    daos_iov_t sg_iov[2];
    uint8_t iov_buf[17];
    uint8_t *p;
    int ret;
    herr_t ret_value = SUCCEED;

    /* Check for write access */
    if(!(grp->obj.item.file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "no write intent on file")

    /* Set up dkey */
    daos_iov_set(&dkey, (void *)name, (daos_size_t)name_len);

    /* Encode link type */
    p = iov_buf;
    *p++ = (uint8_t)val->type;

    /* Initialized iod */
    memset(&iod, 0, sizeof(iod));

    /* Encode type specific value information */
    switch(val->type) {
         case H5L_TYPE_HARD:
            assert(sizeof(iov_buf) == sizeof(val->target.hard) + 1);

            /* Encode oid */
            UINT64ENCODE(p, val->target.hard.lo)
            UINT64ENCODE(p, val->target.hard.hi)

            iod.iod_size = (uint64_t)17;

            /* Set up type specific sgl */
            daos_iov_set(&sg_iov[0], iov_buf, (daos_size_t)sizeof(iov_buf));
            sgl.sg_nr = 1;
            sgl.sg_nr_out = 0;

            break;

        case H5L_TYPE_SOFT:
            /* We need an extra byte for the link type (encoded above). */
            iod.iod_size = (uint64_t)(strlen(val->target.soft) + 1);

            /* Set up type specific sgl.  We use two entries, the first for the
             * link type, the second for the string. */
            daos_iov_set(&sg_iov[0], iov_buf, (daos_size_t)1);
            daos_iov_set(&sg_iov[1], val->target.soft, (daos_size_t)(iod.iod_size - (uint64_t)1));
            sgl.sg_nr = 2;
            sgl.sg_nr_out = 0;

            break;

        case H5L_TYPE_ERROR:
        case H5L_TYPE_EXTERNAL:
        case H5L_TYPE_MAX:
        default:
            D_GOTO_ERROR(H5E_SYM, H5E_BADVALUE, FAIL, "invalid or unsupported link type")
    } /* end switch */


    /* Finish setting up iod */
    daos_iov_set(&iod.iod_name, const_link_key, (daos_size_t)(sizeof(const_link_key) - 1));
    daos_csum_set(&iod.iod_kcsum, NULL, 0);
    iod.iod_nr = 1u;
    iod.iod_type = DAOS_IOD_SINGLE;

    /* Set up general sgl */
    sgl.sg_iovs = sg_iov;

    /* Write link */
    if(0 != (ret = daos_obj_update(grp->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't write link: %d", ret)

done:
    D_FUNC_LEAVE
} /* end H5_daos_link_write() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_create
 *
 * Purpose:     Creates a hard/soft/UD/external links.
 *              For now, only Soft Links are Supported.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_create(H5VL_link_create_type_t create_type, void *_item,
    const H5VL_loc_params_t *loc_params, hid_t lcpl_id, hid_t DV_ATTR_UNUSED lapl_id,
    hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_group_t *link_grp = NULL;
    const char *link_name = NULL;
    H5_daos_link_val_t link_val;
    herr_t ret_value = SUCCEED;

    assert(loc_params->type == H5VL_OBJECT_BY_NAME);

    /* Find target group */
    if(item)
        if(NULL == (link_grp = H5_daos_group_traverse(item, loc_params->loc_data.loc_by_name.name, dxpl_id, req, &link_name, NULL, NULL)))
            D_GOTO_ERROR(H5E_SYM, H5E_BADITER, FAIL, "can't traverse path")

    switch(create_type) {
        case H5VL_LINK_CREATE_HARD:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "hard link creation not supported")

            break;

        case H5VL_LINK_CREATE_SOFT:
            /* Retrieve target name */
            link_val.type = H5L_TYPE_SOFT;
            if(H5Pget(lcpl_id, H5VL_PROP_LINK_TARGET_NAME, &link_val.target.soft) < 0)
                D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "can't get property value for target name")

            /* Create soft link */
            if(H5_daos_link_write(link_grp, link_name, strlen(link_name), &link_val) < 0)
                D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't create soft link")

            break;

        case H5VL_LINK_CREATE_UD:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "UD link creation not supported")
        default:
            D_GOTO_ERROR(H5E_LINK, H5E_CANTINIT, FAIL, "invalid link creation call")
    } /* end switch */

done:
    /* Close link group */
    if(link_grp && H5_daos_group_close(link_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_link_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_specific
 *
 * Purpose:     Specific operations with links
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_specific(void *_item, const H5VL_loc_params_t *loc_params,
    H5VL_link_specific_t specific_type, hid_t dxpl_id, void **req,
    va_list arguments)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_group_t *target_grp = NULL;
    hid_t target_grp_id = -1;
    char *dkey_buf = NULL;
    size_t dkey_buf_len = 0;
    int ret;
    herr_t ret_value = SUCCEED;    /* Return value */

    switch (specific_type) {
        /* H5Lexists */
        case H5VL_LINK_EXISTS:
            {
                htri_t *lexists_ret = va_arg(arguments, htri_t *);
                const char *target_name = NULL;
                char const_link_key[] = H5_DAOS_LINK_KEY;
                daos_key_t dkey;
                daos_iod_t iod;

                assert(H5VL_OBJECT_BY_NAME == loc_params->type);

                /* Traverse the path */
                if(NULL == (target_grp = H5_daos_group_traverse(item, loc_params->loc_data.loc_by_name.name, dxpl_id, req, &target_name, NULL, NULL)))
                    D_GOTO_ERROR(H5E_SYM, H5E_BADITER, FAIL, "can't traverse path")

                /* Set up dkey */
                daos_iov_set(&dkey, (void *)target_name, strlen(target_name));

                /* Set up iod */
                memset(&iod, 0, sizeof(iod));
                daos_iov_set(&iod.iod_name, const_link_key, (daos_size_t)(sizeof(const_link_key) - 1));
                daos_csum_set(&iod.iod_kcsum, NULL, 0);
                iod.iod_nr = 1u;
                iod.iod_size = DAOS_REC_ANY;
                iod.iod_type = DAOS_IOD_SINGLE;

                /* Read link */
                if(0 != (ret = daos_obj_fetch(target_grp->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, NULL /*sgl*/, NULL /*maps*/, NULL /*event*/)))
                    D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't read link: %d", ret)

                /* Set return value */
                *lexists_ret = iod.iod_size != (uint64_t)0;

                break;
            } /* end block */

#ifdef DV_HAVE_LINK_ITERATION
        case H5VL_LINK_ITER:
            {
                hbool_t recursive = va_arg(arguments, int);
                H5_index_t DV_ATTR_UNUSED idx_type = (H5_index_t)va_arg(arguments, int);
                H5_iter_order_t order = (H5_iter_order_t)va_arg(arguments, int);
                hsize_t *idx = va_arg(arguments, hsize_t *);
                H5L_iterate_t op = va_arg(arguments, H5L_iterate_t);
                void *op_data = va_arg(arguments, void *);
                daos_anchor_t anchor;
                uint32_t nr;
                daos_key_desc_t kds[H5_DAOS_ITER_LEN];
                daos_sg_list_t sgl;
                daos_iov_t sg_iov;
                H5_daos_link_val_t link_val;
                H5L_info_t linfo;
                herr_t op_ret;
                char tmp_char;
                char *p;
                uint32_t i;

                /* Determine the target group */
                if(loc_params->type == H5VL_OBJECT_BY_SELF) {
                    /* Use item as attribute parent object, or the root group if item is a
                     * file */
                    if(item->type == H5I_GROUP)
                        target_grp = (H5_daos_group_t *)item;
                    else if(item->type == H5I_FILE)
                        target_grp = ((H5_daos_file_t *)item)->root_grp;
                    else
                        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "item not a file or group")
                    target_grp->obj.item.rc++;
                } /* end if */
                else if(loc_params->type == H5VL_OBJECT_BY_NAME) {
                    H5VL_loc_params_t sub_loc_params;

                    /* Open target_grp */
                    sub_loc_params.obj_type = item->type;
                    sub_loc_params.type = H5VL_OBJECT_BY_SELF;
                    if(NULL == (target_grp = (H5_daos_group_t *)H5_daos_group_open(item, &sub_loc_params, loc_params->loc_data.loc_by_name.name, loc_params->loc_data.loc_by_name.lapl_id, dxpl_id, req)))
                        D_GOTO_ERROR(H5E_SYM, H5E_CANTOPENOBJ, FAIL, "can't open group for link operation")
                } /* end else */

                /* Iteration restart not supported */
                if(idx && (*idx != 0))
                    D_GOTO_ERROR(H5E_SYM, H5E_UNSUPPORTED, FAIL, "iteration restart not supported (must start from 0)")

                /* Ordered iteration not supported */
                if(order != H5_ITER_NATIVE)
                    D_GOTO_ERROR(H5E_SYM, H5E_UNSUPPORTED, FAIL, "ordered iteration not supported (order must be H5_ITER_NATIVE)")

                /* Recursive iteration not supported */
                if(recursive)
                    D_GOTO_ERROR(H5E_SYM, H5E_UNSUPPORTED, FAIL, "recusive iteration not supported")

                /* Initialize const linfo info */
                linfo.corder_valid = FALSE;
                linfo.corder = 0;
                linfo.cset = H5T_CSET_ASCII;

                /* Register id for target_grp */
                if((target_grp_id = H5VLregister(H5I_GROUP, target_grp, H5_DAOS_g)) < 0)
                    D_GOTO_ERROR(H5E_ATOM, H5E_CANTREGISTER, FAIL, "unable to atomize object handle")

                /* Initialize anchor */
                memset(&anchor, 0, sizeof(anchor));

                /* Allocate dkey_buf */
                if(NULL == (dkey_buf = (char *)DV_malloc(H5_DAOS_ITER_SIZE_INIT)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for dkeys")
                dkey_buf_len = H5_DAOS_ITER_SIZE_INIT;

                /* Set up sgl.  Report size as 1 less than buffer size so we
                 * always have room for a null terminator. */
                daos_iov_set(&sg_iov, dkey_buf, (daos_size_t)(dkey_buf_len - 1));
                sgl.sg_nr = 1;
                sgl.sg_nr_out = 0;
                sgl.sg_iovs = &sg_iov;

                /* Loop to retrieve keys and make callbacks */
                do {
                    /* Loop to retrieve keys (exit as soon as we get at least 1
                     * key) */
                    do {
                        /* Reset nr */
                        nr = H5_DAOS_ITER_LEN;

                        /* Ask daos for a list of dkeys, break out if we succeed
                         */
                        if(0 == (ret = daos_obj_list_dkey(target_grp->obj.obj_oh, DAOS_TX_NONE, &nr, kds, &sgl, &anchor, NULL /*event*/)))
                            break;

                        /* Call failed, if the buffer is too small double it and
                         * try again, otherwise fail */
                        if(ret == -DER_KEY2BIG) {
                            /* Allocate larger buffer */
                            DV_free(dkey_buf);
                            dkey_buf_len *= 2;
                            if(NULL == (dkey_buf = (char *)DV_malloc(dkey_buf_len)))
                                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for dkeys")

                            /* Update sgl */
                            daos_iov_set(&sg_iov, dkey_buf, (daos_size_t)(dkey_buf_len - 1));
                        } /* end if */
                        else
                            D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, FAIL, "can't retrieve attributes: %d", ret)
                    } while(1);

                    /* Loop over returned dkeys */
                    p = dkey_buf;
                    op_ret = 0;
                    for(i = 0; (i < nr) && (op_ret == 0); i++) {
                        /* Check if this key represents a link */
                        if(p[0] != '/') {
                            /* Add null terminator temporarily */
                            tmp_char = p[kds[i].kd_key_len];
                            p[kds[i].kd_key_len] = '\0';

                            /* Read link */
                            if(H5_daos_link_read(target_grp, p, (size_t)kds[i].kd_key_len, &link_val) < 0)
                                D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't read link")

                            /* Update linfo */
                            linfo.type = link_val.type;
                            if(link_val.type == H5L_TYPE_HARD)
                                linfo.u.address = (haddr_t)link_val.target.hard.lo;
                            else {
                                assert(link_val.type == H5L_TYPE_SOFT);
                                linfo.u.val_size = strlen(link_val.target.soft) + 1;

                                /* Free soft link value */
                                link_val.target.soft = (char *)DV_free(link_val.target.soft);
                            } /* end else */

                            /* Make callback */
                            if((op_ret = op(target_grp_id, p, &linfo, op_data)) < 0)
                                D_GOTO_ERROR(H5E_SYM, H5E_BADITER, op_ret, "operator function returned failure")

                            /* Replace null terminator */
                            p[kds[i].kd_key_len] = tmp_char;

                            /* Advance idx */
                            if(idx)
                                (*idx)++;
                        } /* end if */

                        /* Advance to next akey */
                        p += kds[i].kd_key_len + kds[i].kd_csum_len;
                    } /* end for */
                } while(!daos_anchor_is_eof(&anchor) && (op_ret == 0));

                /* Set return value */
                ret_value = op_ret;

                break;
            } /* end block */
#endif

        case H5VL_LINK_DELETE:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "unsupported specific operation")
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_BADVALUE, FAIL, "invalid specific operation")
    } /* end switch */

done:
    if(target_grp_id >= 0) {
        if(H5Idec_ref(target_grp_id) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group id")
        target_grp_id = -1;
        target_grp = NULL;
    } /* end if */
    else if(target_grp) {
        if(H5_daos_group_close(target_grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group")
        target_grp = NULL;
    } /* end else */
    dkey_buf = (char *)DV_free(dkey_buf);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_link_specific() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_link_follow
 *
 * Purpose:     Follows the link in grp identified with name, and returns
 *              in oid the oid of the target object.
 *
 * Return:      Success:        SUCCEED 
 *              Failure:        FAIL
 *
 * Programmer:  Neil Fortner
 *              January, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_link_follow(H5_daos_group_t *grp, const char *name,
    size_t name_len, hid_t dxpl_id, void **req, daos_obj_id_t *oid)
{
    H5_daos_link_val_t link_val;
    hbool_t link_val_alloc = FALSE;
    H5_daos_group_t *target_grp = NULL;
    herr_t ret_value = SUCCEED;

    assert(grp);
    assert(name);
    assert(oid);

    /* Read link to group */
   if(H5_daos_link_read(grp, name, name_len, &link_val) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't read link")

    switch(link_val.type) {
       case H5L_TYPE_HARD:
            /* Simply return the read oid */
            *oid = link_val.target.hard;

            break;

        case H5L_TYPE_SOFT:
            {
                const char *target_name = NULL;

                link_val_alloc = TRUE;

                /* Traverse the soft link path */
                if(NULL == (target_grp = H5_daos_group_traverse(&grp->obj.item, link_val.target.soft, dxpl_id, req, &target_name, NULL, NULL)))
                    D_GOTO_ERROR(H5E_SYM, H5E_BADITER, FAIL, "can't traverse path")

                /* Check for no target_name, in this case just return
                 * target_grp's oid */
                if(target_name[0] == '\0'
                        || (target_name[0] == '.' && name_len == (size_t)1))
                    *oid = target_grp->obj.oid;
                else
                    /* Follow the last element in the path */
                    if(H5_daos_link_follow(target_grp, target_name, strlen(target_name), dxpl_id, req, oid) < 0)
                        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't follow link")

                break;
            } /* end block */

        case H5L_TYPE_ERROR:
        case H5L_TYPE_EXTERNAL:
        case H5L_TYPE_MAX:
        default:
           D_GOTO_ERROR(H5E_SYM, H5E_BADVALUE, FAIL, "invalid or unsupported link type")
    } /* end switch */

done:
    /* Clean up */
    if(link_val_alloc) {
        assert(link_val.type == H5L_TYPE_SOFT);
        DV_free(link_val.target.soft);
    } /* end if */

    if(target_grp)
        if(H5_daos_group_close(target_grp, dxpl_id, req) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group")

    D_FUNC_LEAVE
} /* end H5_daos_link_follow() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_group_traverse
 *
 * Purpose:     Given a path name and base object, returns the final group
 *              in the path and the object name.  obj_name points into the
 *              buffer given by path, so it does not need to be freed.
 *              The group must be closed with H5_daos_group_close().
 *
 * Return:      Success:        group object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static H5_daos_group_t *
H5_daos_group_traverse(H5_daos_item_t *item, const char *path,
    hid_t dxpl_id, void **req, const char **obj_name, void **gcpl_buf_out,
    uint64_t *gcpl_len_out)
{
    H5_daos_group_t *grp = NULL;
    const char *next_obj;
    daos_obj_id_t oid;
    H5_daos_group_t *ret_value = NULL;

    assert(item);
    assert(path);
    assert(obj_name);

    /* Initialize obj_name */
    *obj_name = path;

    /* Open starting group */
    if((*obj_name)[0] == '/') {
        grp = item->file->root_grp;
        (*obj_name)++;
    } /* end if */
    else {
        if(item->type == H5I_GROUP)
            grp = (H5_daos_group_t *)item;
        else if(item->type == H5I_FILE)
            grp = ((H5_daos_file_t *)item)->root_grp;
        else
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "item not a file or group")
    } /* end else */
        
    grp->obj.item.rc++;

    /* Search for '/' */
    next_obj = strchr(*obj_name, '/');

    /* Traverse path */
    while(next_obj) {
        /* Free gcpl_buf_out */
        if(gcpl_buf_out)
            *gcpl_buf_out = DV_free(*gcpl_buf_out);

        /* Follow link to next group in path */
        assert(next_obj > *obj_name);
        if(H5_daos_link_follow(grp, *obj_name, (size_t)(next_obj - *obj_name), dxpl_id, req, &oid) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "can't follow link to group")

        /* Close previous group */
        if(H5_daos_group_close(grp, dxpl_id, req) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CLOSEERROR, NULL, "can't close group")
        grp = NULL;

        /* Open group */
        if(NULL == (grp = (H5_daos_group_t *)H5_daos_group_open_helper(item->file, oid, H5P_GROUP_ACCESS_DEFAULT, dxpl_id, req, gcpl_buf_out, gcpl_len_out)))
            D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "can't open group")

        /* Advance to next path element */
        *obj_name = next_obj + 1;
        next_obj = strchr(*obj_name, '/');
    } /* end while */

    /* Set return value */
    ret_value = grp;

done:
    /* Cleanup on failure */
    if(NULL == ret_value)
        /* Close group */
        if(grp && H5_daos_group_close(grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CLOSEERROR, NULL, "can't close group")

    D_FUNC_LEAVE
} /* end H5_daos_group_traverse() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_group_create_helper
 *
 * Purpose:     Performs the actual group creation, but does not create a
 *              link.
 *
 * Return:      Success:        group object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_group_create_helper(H5_daos_file_t *file, hid_t gcpl_id,
    hid_t gapl_id, hid_t dxpl_id, void **req, H5_daos_group_t *parent_grp,
    const char *name, size_t name_len, hbool_t collective)
{
    H5_daos_group_t *grp = NULL;
    void *gcpl_buf = NULL;
    int ret;
    void *ret_value = NULL;

    assert(file->flags & H5F_ACC_RDWR);

    /* Allocate the group object that is returned to the user */
    if(NULL == (grp = H5FL_CALLOC(H5_daos_group_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS group struct")
    grp->obj.item.type = H5I_GROUP;
    grp->obj.item.file = file;
    grp->obj.item.rc = 1;
    grp->obj.obj_oh = DAOS_HDL_INVAL;
    grp->gcpl_id = FAIL;
    grp->gapl_id = FAIL;

    /* Generate group oid */
    H5_daos_oid_encode(&grp->obj.oid, file->max_oid + (uint64_t)1, H5I_GROUP);

    /* Create group and write metadata if this process should */
    if(!collective || (file->my_rank == 0)) {
        daos_key_t dkey;
        daos_iod_t iod;
        daos_sg_list_t sgl;
        daos_iov_t sg_iov;
        size_t gcpl_size = 0;
        char int_md_key[] = H5_DAOS_INT_MD_KEY;
        char gcpl_key[] = H5_DAOS_CPL_KEY;

        /* Create group */
        /* Update max_oid */
        file->max_oid = H5_daos_oid_to_idx(grp->obj.oid);

        /* Write max OID */
        if(H5_daos_write_max_oid(file) < 0)
            D_GOTO_ERROR(H5E_FILE, H5E_CANTINIT, NULL, "can't write max OID")

        /* Open group */
        if(0 != (ret = daos_obj_open(file->coh, grp->obj.oid, DAOS_OO_RW, &grp->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENOBJ, NULL, "can't open group: %d", ret)

        /* Encode GCPL */
        if(H5Pencode(gcpl_id, NULL, &gcpl_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of gcpl")
        if(NULL == (gcpl_buf = DV_malloc(gcpl_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized gcpl")
        if(H5Pencode(gcpl_id, gcpl_buf, &gcpl_size) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CANTENCODE, NULL, "can't serialize gcpl")

        /* Set up operation to write GCPL to group */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)gcpl_key, (daos_size_t)(sizeof(gcpl_key) - 1));
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_nr = 1u;
        iod.iod_size = (uint64_t)gcpl_size;
        iod.iod_type = DAOS_IOD_SINGLE;

        /* Set up sgl */
        daos_iov_set(&sg_iov, gcpl_buf, (daos_size_t)gcpl_size);
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &sg_iov;

        /* Write internal metadata to group */
        if(0 != (ret = daos_obj_update(grp->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "can't write metadata to group: %d", ret)

        /* Write link to group if requested */
        if(parent_grp) {
            H5_daos_link_val_t link_val;

            link_val.type = H5L_TYPE_HARD;
            link_val.target.hard = grp->obj.oid;
            if(H5_daos_link_write(parent_grp, name, name_len, &link_val) < 0)
                D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "can't create link to group")
        } /* end if */
    } /* end if */
    else {
        /* Update max_oid */
        file->max_oid = grp->obj.oid.lo;

        /* Note no barrier is currently needed here, daos_obj_open is a local
         * operation and can occur before the lead process writes metadata.  For
         * app-level synchronization we could add a barrier or bcast to the
         * calling functions (file_create, group_create) though it could only be
         * an issue with group reopen so we'll skip it for now.  There is
         * probably never an issue with file reopen since all commits are from
         * process 0, same as the group create above. */

        /* Open group */
        if(0 != (ret = daos_obj_open(file->coh, grp->obj.oid, DAOS_OO_RW, &grp->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENOBJ, NULL, "can't open group: %d", ret)
    } /* end else */

    /* Finish setting up group struct */
    if((grp->gcpl_id = H5Pcopy(gcpl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy gcpl");
    if((grp->gapl_id = H5Pcopy(gapl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy gapl");

    ret_value = (void *)grp;

done:
    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close group */
        if(grp && H5_daos_group_close(grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_FILE, H5E_CLOSEERROR, NULL, "can't close group")

    /* Free memory */
    gcpl_buf = DV_free(gcpl_buf);

    D_FUNC_LEAVE
} /* end H5_daos_group_create_helper() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_group_create
 *
 * Purpose:     Sends a request to DAOS to create a group
 *
 * Return:      Success:        group object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_group_create(void *_item,
    const H5VL_loc_params_t DV_ATTR_UNUSED *loc_params, const char *name,
    hid_t gcpl_id, hid_t gapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_group_t *grp = NULL;
    H5_daos_group_t *target_grp = NULL;
    const char *target_name = NULL;
    hbool_t collective = item->file->collective;
    void *ret_value = NULL;

    /* Check for write access */
    if(!(item->file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, NULL, "no write intent on file")
 
    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(gapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, NULL, "can't get collective access property")

    /* Traverse the path */
    if(name && (!collective || (item->file->my_rank == 0)))
        if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
            D_GOTO_ERROR(H5E_SYM, H5E_BADITER, NULL, "can't traverse path")

    /* Create group and link to group */
    if(NULL == (grp = (H5_daos_group_t *)H5_daos_group_create_helper(item->file, gcpl_id, gapl_id, dxpl_id, req, target_grp, target_name, target_name ? strlen(target_name) : 0, collective)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "can't create group")

    /* Set return value */
    ret_value = (void *)grp;

done:
    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, NULL, "can't close group")

    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close group */
        if(grp && H5_daos_group_close(grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, NULL, "can't close group")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_group_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_group_open_helper
 *
 * Purpose:     Performs the actual group open, given the oid.
 *
 * Return:      Success:        group object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_group_open_helper(H5_daos_file_t *file, daos_obj_id_t oid,
    hid_t gapl_id, hid_t dxpl_id, void **req, void **gcpl_buf_out,
    uint64_t *gcpl_len_out)
{
    H5_daos_group_t *grp = NULL;
    daos_key_t dkey;
    daos_iod_t iod;
    daos_sg_list_t sgl;
    daos_iov_t sg_iov;
    void *gcpl_buf = NULL;
    char int_md_key[] = H5_DAOS_INT_MD_KEY;
    char gcpl_key[] = H5_DAOS_CPL_KEY;
    uint64_t gcpl_len;
    int ret;
    void *ret_value = NULL;

    /* Allocate the group object that is returned to the user */
    if(NULL == (grp = H5FL_CALLOC(H5_daos_group_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS group struct")
    grp->obj.item.type = H5I_GROUP;
    grp->obj.item.file = file;
    grp->obj.item.rc = 1;
    grp->obj.oid = oid;
    grp->obj.obj_oh = DAOS_HDL_INVAL;
    grp->gcpl_id = FAIL;
    grp->gapl_id = FAIL;

    /* Open group */
    if(0 != (ret = daos_obj_open(file->coh, oid, file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &grp->obj.obj_oh, NULL /*event*/)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENOBJ, NULL, "can't open group: %d", ret)

    /* Set up operation to read GCPL size from group */
    /* Set up dkey */
    daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

    /* Set up iod */
    memset(&iod, 0, sizeof(iod));
    daos_iov_set(&iod.iod_name, (void *)gcpl_key, (daos_size_t)(sizeof(gcpl_key) - 1));
    daos_csum_set(&iod.iod_kcsum, NULL, 0);
    iod.iod_nr = 1u;
    iod.iod_size = DAOS_REC_ANY;
    iod.iod_type = DAOS_IOD_SINGLE;

    /* Read internal metadata size from group */
    if(0 != (ret = daos_obj_fetch(grp->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, NULL, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTDECODE, NULL, "can't read metadata size from group: %d", ret)

    /* Check for metadata not found */
    if(iod.iod_size == (uint64_t)0)
        D_GOTO_ERROR(H5E_SYM, H5E_NOTFOUND, NULL, "internal metadata not found")

    /* Allocate buffer for GCPL */
    gcpl_len = iod.iod_size;
    if(NULL == (gcpl_buf = DV_malloc(gcpl_len)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized gcpl")

    /* Set up sgl */
    daos_iov_set(&sg_iov, gcpl_buf, (daos_size_t)gcpl_len);
    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &sg_iov;

    /* Read internal metadata from group */
    if(0 != (ret = daos_obj_fetch(grp->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_SYM, H5E_CANTDECODE, NULL, "can't read metadata from group: %d", ret)

    /* Decode GCPL */
    if((grp->gcpl_id = H5Pdecode(gcpl_buf)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize GCPL")

    /* Finish setting up group struct */
    if((grp->gapl_id = H5Pcopy(gapl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy gapl");

    /* Return GCPL info if requested, relinquish ownership of gcpl_buf if so */
    if(gcpl_buf_out) {
        assert(gcpl_len_out);
        assert(!*gcpl_buf_out);

        *gcpl_buf_out = gcpl_buf;
        gcpl_buf = NULL;

        *gcpl_len_out = gcpl_len;
    } /* end if */

    ret_value = (void *)grp;

done:
    /* Cleanup on failure */
    if(NULL == ret_value)
        /* Close group */
        if(grp && H5_daos_group_close(grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, NULL, "can't close group")

    /* Free memory */
    gcpl_buf = DV_free(gcpl_buf);

    D_FUNC_LEAVE
} /* end H5_daos_group_open_helper() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_group_reconstitute
 *
 * Purpose:     Reconstitutes a group object opened by another process.
 *
 * Return:      Success:        group object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              April, 2017
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_group_reconstitute(H5_daos_file_t *file, daos_obj_id_t oid,
    uint8_t *gcpl_buf, hid_t gapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_group_t *grp = NULL;
    int ret;
    void *ret_value = NULL;

    /* Allocate the group object that is returned to the user */
    if(NULL == (grp = H5FL_CALLOC(H5_daos_group_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS group struct")
    grp->obj.item.type = H5I_GROUP;
    grp->obj.item.file = file;
    grp->obj.item.rc = 1;
    grp->obj.oid = oid;
    grp->obj.obj_oh = DAOS_HDL_INVAL;
    grp->gcpl_id = FAIL;
    grp->gapl_id = FAIL;

    /* Open group */
    if(0 != (ret = daos_obj_open(file->coh, oid, file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &grp->obj.obj_oh, NULL /*event*/)))
        D_GOTO_ERROR(H5E_FILE, H5E_CANTOPENOBJ, NULL, "can't open group: %d", ret)

    /* Decode GCPL */
    if((grp->gcpl_id = H5Pdecode(gcpl_buf)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize GCPL")

    /* Finish setting up group struct */
    if((grp->gapl_id = H5Pcopy(gapl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy gapl");

    ret_value = (void *)grp;

done:
    /* Cleanup on failure */
    if(NULL == ret_value)
        /* Close group */
        if(grp && H5_daos_group_close(grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, NULL, "can't close group")

    D_FUNC_LEAVE
} /* end H5_daos_group_reconstitute() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_group_open
 *
 * Purpose:     Sends a request to DAOS to open a group
 *
 * Return:      Success:        dataset object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_group_open(void *_item, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t gapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_group_t *grp = NULL;
    H5_daos_group_t *target_grp = NULL;
    const char *target_name = NULL;
    daos_obj_id_t oid;
    uint8_t *gcpl_buf = NULL;
    uint64_t gcpl_len = 0;
    uint8_t ginfo_buf_static[H5_DAOS_GINFO_BUF_SIZE];
    uint8_t *p;
    hbool_t collective = item->file->collective;
    hbool_t must_bcast = FALSE;
    void *ret_value = NULL;
 
    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(gapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, NULL, "can't get collective access property")

    /* Check if we're actually opening the group or just receiving the group
     * info from the leader */
    if(!collective || (item->file->my_rank == 0)) {
        if(collective && (item->file->num_procs > 1))
            must_bcast = TRUE;

        /* Check for open by address */
        if(H5VL_OBJECT_BY_ADDR == loc_params->type) {
            /* Generate oid from address */
            memset(&oid, 0, sizeof(oid));
            H5_daos_oid_generate(&oid, (uint64_t)loc_params->loc_data.loc_by_addr.addr, H5I_GROUP);

            /* Open group */
            if(NULL == (grp = (H5_daos_group_t *)H5_daos_group_open_helper(item->file, oid, gapl_id, dxpl_id, req, (collective && (item->file->num_procs > 1)) ? (void **)&gcpl_buf : NULL, &gcpl_len)))
                D_GOTO_ERROR(H5E_SYM, H5E_CANTOPENOBJ, NULL, "can't open group")
        } /* end if */
        else {
            /* Open using name parameter */
            /* Traverse the path */
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, (collective && (item->file->num_procs > 1)) ? (void **)&gcpl_buf : NULL, &gcpl_len)))
                D_GOTO_ERROR(H5E_SYM, H5E_BADITER, NULL, "can't traverse path")

            /* Check for no target_name, in this case just return target_grp */
            if(target_name[0] == '\0'
                    || (target_name[0] == '.' && target_name[1] == '\0')) {
                size_t gcpl_size;

                /* Take ownership of target_grp */
                grp = target_grp;
                target_grp = NULL;

                /* Encode GCPL */
                if(H5Pencode(grp->gcpl_id, NULL, &gcpl_size) < 0)
                    D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of gcpl")
                if(NULL == (gcpl_buf = (uint8_t *)DV_malloc(gcpl_size)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized gcpl")
                gcpl_len = (uint64_t)gcpl_size;
                if(H5Pencode(grp->gcpl_id, gcpl_buf, &gcpl_size) < 0)
                    D_GOTO_ERROR(H5E_SYM, H5E_CANTENCODE, NULL, "can't serialize gcpl")
            } /* end if */
            else {
                gcpl_buf = (uint8_t *)DV_free(gcpl_buf);
                gcpl_len = 0;

                /* Follow link to group */
                if(H5_daos_link_follow(target_grp, target_name, strlen(target_name), dxpl_id, req, &oid) < 0)
                    D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "can't follow link to group")

                /* Open group */
                if(NULL == (grp = (H5_daos_group_t *)H5_daos_group_open_helper(item->file, oid, gapl_id, dxpl_id, req, (collective && (item->file->num_procs > 1)) ? (void **)&gcpl_buf : NULL, &gcpl_len)))
                    D_GOTO_ERROR(H5E_SYM, H5E_CANTOPENOBJ, NULL, "can't open group")
            } /* end else */
        } /* end else */

        /* Broadcast group info if there are other processes that need it */
        if(collective && (item->file->num_procs > 1)) {
            assert(gcpl_buf);
            assert(sizeof(ginfo_buf_static) >= 3 * sizeof(uint64_t));

            /* Encode oid */
            p = ginfo_buf_static;
            UINT64ENCODE(p, grp->obj.oid.lo)
            UINT64ENCODE(p, grp->obj.oid.hi)

            /* Encode GCPL length */
            UINT64ENCODE(p, gcpl_len)

            /* Copy GCPL to ginfo_buf_static if it will fit */
            if((gcpl_len + 3 * sizeof(uint64_t)) <= sizeof(ginfo_buf_static))
                (void)memcpy(p, gcpl_buf, gcpl_len);

            /* We are about to bcast so we no longer need to bcast on failure */
            must_bcast = FALSE;

            /* MPI_Bcast ginfo_buf */
            if(MPI_SUCCESS != MPI_Bcast((char *)ginfo_buf_static, sizeof(ginfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_SYM, H5E_MPI, NULL, "can't bcast group info")

            /* Need a second bcast if it did not fit in the receivers' static
             * buffer */
            if(gcpl_len + 3 * sizeof(uint64_t) > sizeof(ginfo_buf_static))
                if(MPI_SUCCESS != MPI_Bcast((char *)gcpl_buf, (int)gcpl_len, MPI_BYTE, 0, item->file->comm))
                    D_GOTO_ERROR(H5E_SYM, H5E_MPI, NULL, "can't bcast GCPL")
        } /* end if */
    } /* end if */
    else {
        /* Receive GCPL */
        if(MPI_SUCCESS != MPI_Bcast((char *)ginfo_buf_static, sizeof(ginfo_buf_static), MPI_BYTE, 0, item->file->comm))
            D_GOTO_ERROR(H5E_SYM, H5E_MPI, NULL, "can't bcast group info")

        /* Decode oid */
        p = ginfo_buf_static;
        UINT64DECODE(p, oid.lo)
        UINT64DECODE(p, oid.hi)

        /* Decode GCPL length */
        UINT64DECODE(p, gcpl_len)

        /* Check for gcpl_len set to 0 - indicates failure */
        if(gcpl_len == 0)
            D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "lead process failed to open group")

        /* Check if we need to perform another bcast */
        if(gcpl_len + 3 * sizeof(uint64_t) > sizeof(ginfo_buf_static)) {
            /* Allocate a dynamic buffer if necessary */
            if(gcpl_len > sizeof(ginfo_buf_static)) {
                if(NULL == (gcpl_buf = (uint8_t *)DV_malloc(gcpl_len)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate space for global pool handle")
                p = gcpl_buf;
            } /* end if */
            else
                p = ginfo_buf_static;

            /* Receive GCPL */
            if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)gcpl_len, MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_SYM, H5E_MPI, NULL, "can't bcast GCPL")
        } /* end if */

        /* Reconstitute group from received oid and GCPL buffer */
        if(NULL == (grp = (H5_daos_group_t *)H5_daos_group_reconstitute(item->file, oid, p, gapl_id, dxpl_id, req)))
            D_GOTO_ERROR(H5E_SYM, H5E_CANTINIT, NULL, "can't reconstitute group")
    } /* end else */

    /* Set return value */
    ret_value = (void *)grp;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast gcpl_buf as '0' if necessary - this will trigger failures in
         * other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(ginfo_buf_static, 0, sizeof(ginfo_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(ginfo_buf_static, sizeof(ginfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_DONE_ERROR(H5E_SYM, H5E_MPI, NULL, "can't bcast empty group info")
        } /* end if */

        /* Close group */
        if(grp && H5_daos_group_close(grp, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, NULL, "can't close group")
    } /* end if */

    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_SYM, H5E_CLOSEERROR, NULL, "can't close group")

    /* Free memory */
    gcpl_buf = (uint8_t *)DV_free(gcpl_buf);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_group_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_group_close
 *
 * Purpose:     Closes a daos HDF5 group.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_group_close(void *_grp, hid_t DV_ATTR_UNUSED dxpl_id,
    void DV_ATTR_UNUSED **req)
{
    H5_daos_group_t *grp = (H5_daos_group_t *)_grp;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(grp);

    if(--grp->obj.item.rc == 0) {
        /* Free group data structures */
        if(!daos_handle_is_inval(grp->obj.obj_oh))
            if(0 != (ret = daos_obj_close(grp->obj.obj_oh, NULL /*event*/)))
                D_DONE_ERROR(H5E_SYM, H5E_CANTCLOSEOBJ, FAIL, "can't close group DAOS object: %d", ret)
        if(grp->gcpl_id != FAIL && H5Idec_ref(grp->gcpl_id) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist")
        if(grp->gapl_id != FAIL && H5Idec_ref(grp->gapl_id) < 0)
            D_DONE_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist")
        grp = H5FL_FREE(H5_daos_group_t, grp);
    } /* end if */

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_group_close() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_need_bkg
 *
 * Purpose:     Determine if a background buffer is needed for conversion.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static htri_t
H5_daos_need_bkg(hid_t src_type_id, hid_t dst_type_id, size_t *dst_type_size,
    hbool_t *fill_bkg)
{
    hid_t memb_type_id = -1;
    hid_t src_memb_type_id = -1;
    char *memb_name = NULL;
    size_t memb_size;
    H5T_class_t tclass;
    htri_t ret_value;

    /* Get destination type size */
    if((*dst_type_size = H5Tget_size(dst_type_id)) == 0)
        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get source type size")

    /* Get datatype class */
    if(H5T_NO_CLASS == (tclass = H5Tget_class(dst_type_id)))
        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get type class")

    switch(tclass) {
        case H5T_INTEGER:
        case H5T_FLOAT:
        case H5T_TIME:
        case H5T_STRING:
        case H5T_BITFIELD:
        case H5T_OPAQUE:
        case H5T_ENUM:
            /* No background buffer necessary */
            ret_value = FALSE;

            break;

        case H5T_COMPOUND:
            {
                int nmemb;
                size_t size_used = 0;
                int src_i;
                int i;

                /* We must always provide a background buffer for compound
                 * conversions.  Only need to check further to see if it must be
                 * filled. */
                ret_value = TRUE;

                /* Get number of compound members */
                if((nmemb = H5Tget_nmembers(dst_type_id)) < 0)
                    D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get number of destination compound members")

                /* Iterate over compound members, checking for a member in
                 * dst_type_id with no match in src_type_id */
                for(i = 0; i < nmemb; i++) {
                    /* Get member type */
                    if((memb_type_id = H5Tget_member_type(dst_type_id, (unsigned)i)) < 0)
                        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get compound member type")

                    /* Get member name */
                    if(NULL == (memb_name = H5Tget_member_name(dst_type_id, (unsigned)i)))
                        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get compound member name")

                    /* Check for matching name in source type */
                    H5E_BEGIN_TRY {
                        src_i = H5Tget_member_index(src_type_id, memb_name);
                    } H5E_END_TRY

                    /* Free memb_name */
                    if(H5free_memory(memb_name) < 0)
                        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTFREE, FAIL, "can't free member name")
                    memb_name = NULL;

                    /* If no match was found, this type is not being filled in,
                     * so we must fill the background buffer */
                    if(src_i < 0) {
                        if(H5Tclose(memb_type_id) < 0)
                            D_GOTO_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, FAIL, "can't close member type")
                        memb_type_id = -1;
                        *fill_bkg = TRUE;
                        D_GOTO_DONE(TRUE)
                    } /* end if */

                    /* Open matching source type */
                    if((src_memb_type_id = H5Tget_member_type(src_type_id, (unsigned)src_i)) < 0)
                        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get compound member type")

                    /* Recursively check member type, this will fill in the
                     * member size */
                    if(H5_daos_need_bkg(src_memb_type_id, memb_type_id, &memb_size, fill_bkg) < 0)
                        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, FAIL, "can't check if background buffer needed")

                    /* Close source member type */
                    if(H5Tclose(src_memb_type_id) < 0)
                        D_GOTO_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, FAIL, "can't close member type")
                    src_memb_type_id = -1;

                    /* Close member type */
                    if(H5Tclose(memb_type_id) < 0)
                        D_GOTO_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, FAIL, "can't close member type")
                    memb_type_id = -1;

                    /* If the source member type needs the background filled, so
                     * does the parent */
                    if(*fill_bkg)
                        D_GOTO_DONE(TRUE)

                    /* Keep track of the size used in compound */
                    size_used += memb_size;
                } /* end for */

                /* Check if all the space in the type is used.  If not, we must
                 * fill the background buffer. */
                /* TODO: This is only necessary on read, we don't care about
                 * compound gaps in the "file" DSINC */
                assert(size_used <= *dst_type_size);
                if(size_used != *dst_type_size)
                    *fill_bkg = TRUE;

                break;
            } /* end block */

        case H5T_ARRAY:
            /* Get parent type */
            if((memb_type_id = H5Tget_super(dst_type_id)) < 0)
                D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get array parent type")

            /* Get source parent type */
            if((src_memb_type_id = H5Tget_super(src_type_id)) < 0)
                D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get array parent type")

            /* Recursively check parent type */
            if((ret_value = H5_daos_need_bkg(src_memb_type_id, memb_type_id, &memb_size, fill_bkg)) < 0)
                D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, FAIL, "can't check if background buffer needed")

            /* Close source parent type */
            if(H5Tclose(src_memb_type_id) < 0)
                D_GOTO_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, FAIL, "can't close array parent type")
            src_memb_type_id = -1;

            /* Close parent type */
            if(H5Tclose(memb_type_id) < 0)
                D_GOTO_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, FAIL, "can't close array parent type")
            memb_type_id = -1;

            break;

        case H5T_REFERENCE:
        case H5T_VLEN:
            /* Not yet supported */
            D_GOTO_ERROR(H5E_DATATYPE, H5E_UNSUPPORTED, FAIL, "reference and vlen types not supported")

            break;

        case H5T_NO_CLASS:
        case H5T_NCLASSES:
        default:
            D_GOTO_ERROR(H5E_DATATYPE, H5E_BADVALUE, FAIL, "invalid type class")
    } /* end switch */

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        if(memb_type_id >= 0)
            if(H5Idec_ref(memb_type_id) < 0)
                D_DONE_ERROR(H5E_DATATYPE, H5E_CANTDEC, FAIL, "failed to close member type")
        if(src_memb_type_id >= 0)
            if(H5Idec_ref(src_memb_type_id) < 0)
                D_DONE_ERROR(H5E_DATATYPE, H5E_CANTDEC, FAIL, "failed to close source member type")
        memb_name = (char *)DV_free(memb_name);
    } /* end if */

    D_FUNC_LEAVE
} /* end H5_daos_need_bkg() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_tconv_init
 *
 * Purpose:     DSINC
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_tconv_init(hid_t src_type_id, size_t *src_type_size,
    hid_t dst_type_id, size_t *dst_type_size, size_t num_elem, void **tconv_buf,
    void **bkg_buf, H5_daos_tconv_reuse_t *reuse, hbool_t *fill_bkg)
{
    htri_t need_bkg;
    htri_t types_equal;
    herr_t ret_value = SUCCEED;

    assert(src_type_size);
    assert(dst_type_size);
    assert(tconv_buf);
    assert(!*tconv_buf);
    assert(bkg_buf);
    assert(!*bkg_buf);
    assert(fill_bkg);
    assert(!*fill_bkg);

    /* Get source type size */
    if((*src_type_size = H5Tget_size(src_type_id)) == 0)
        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get source type size")

    /* Check if the types are equal */
    if((types_equal = H5Tequal(src_type_id, dst_type_id)) < 0)
        D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTCOMPARE, FAIL, "can't check if types are equal")
    if(types_equal)
        /* Types are equal, no need for conversion, just set dst_type_size */
        *dst_type_size = *src_type_size;
    else {
        /* Check if we need a background buffer */
        if((need_bkg = H5_daos_need_bkg(src_type_id, dst_type_id, dst_type_size, fill_bkg)) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, FAIL, "can't check if background buffer needed")

        /* Check for reusable destination buffer */
        if(reuse) {
            assert(*reuse == H5_DAOS_TCONV_REUSE_NONE);

            /* Use dest buffer for type conversion if it large enough, otherwise
             * use it for the background buffer if one is needed. */
            if(dst_type_size >= src_type_size)
                *reuse = H5_DAOS_TCONV_REUSE_TCONV;
            else if(need_bkg)
                *reuse = H5_DAOS_TCONV_REUSE_BKG;
        } /* end if */

        /* Allocate conversion buffer if it is not being reused */
        if(!reuse || (*reuse != H5_DAOS_TCONV_REUSE_TCONV))
            if(NULL == (*tconv_buf = DV_malloc(num_elem * (*src_type_size
                    > *dst_type_size ? *src_type_size : *dst_type_size))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate type conversion buffer")

        /* Allocate background buffer if one is needed and it is not being
         * reused */
        if(need_bkg && (!reuse || (*reuse != H5_DAOS_TCONV_REUSE_BKG)))
            if(NULL == (*bkg_buf = DV_calloc(num_elem * *dst_type_size)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate background buffer")
    } /* end else */

done:
    /* Cleanup on failure */
    if(ret_value < 0) {
        *tconv_buf = DV_free(*tconv_buf);
        *bkg_buf = DV_free(*bkg_buf);
        if(reuse)
            *reuse = H5_DAOS_TCONV_REUSE_NONE;
    } /* end if */

    D_FUNC_LEAVE
} /* end H5_daos_tconv_init() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_create
 *
 * Purpose:     Sends a request to DAOS to create a dataset
 *
 * Return:      Success:        dataset object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_dataset_create(void *_item,
    const H5VL_loc_params_t DV_ATTR_UNUSED *loc_params, const char *name,
    hid_t dcpl_id, hid_t dapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_dset_t *dset = NULL;
    hid_t type_id, space_id;
    H5_daos_group_t *target_grp = NULL;
    void *type_buf = NULL;
    void *space_buf = NULL;
    void *dcpl_buf = NULL;
    hbool_t collective = item->file->collective;
    int ret;
    void *ret_value = NULL;

    /* Check for write access */
    if(!(item->file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, NULL, "no write intent on file")
 
    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(dapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, NULL, "can't get collective access property")

    /* get creation properties */
    if(H5Pget(dcpl_id, H5VL_PROP_DSET_TYPE_ID, &type_id) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, NULL, "can't get property value for datatype id")
    if(H5Pget(dcpl_id, H5VL_PROP_DSET_SPACE_ID, &space_id) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, NULL, "can't get property value for space id")

    /* Allocate the dataset object that is returned to the user */
    if(NULL == (dset = H5FL_CALLOC(H5_daos_dset_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS dataset struct")
    dset->obj.item.type = H5I_DATASET;
    dset->obj.item.file = item->file;
    dset->obj.item.rc = 1;
    dset->obj.obj_oh = DAOS_HDL_INVAL;
    dset->type_id = FAIL;
    dset->space_id = FAIL;
    dset->dcpl_id = FAIL;
    dset->dapl_id = FAIL;

    /* Generate dataset oid */
    H5_daos_oid_encode(&dset->obj.oid, item->file->max_oid + (uint64_t)1, H5I_DATASET);

    /* Create dataset and write metadata if this process should */
    if(!collective || (item->file->my_rank == 0)) {
        const char *target_name = NULL;
        H5_daos_link_val_t link_val;
        daos_key_t dkey;
        daos_iod_t iod[3];
        daos_sg_list_t sgl[3];
        daos_iov_t sg_iov[3];
        size_t type_size = 0;
        size_t space_size = 0;
        size_t dcpl_size = 0;
        char int_md_key[] = H5_DAOS_INT_MD_KEY;
        char type_key[] = H5_DAOS_TYPE_KEY;
        char space_key[] = H5_DAOS_SPACE_KEY;
        char dcpl_key[] = H5_DAOS_CPL_KEY;

        /* Traverse the path */
        if(name)
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, NULL, "can't traverse path")

        /* Create dataset */
        /* Update max_oid */
        item->file->max_oid = H5_daos_oid_to_idx(dset->obj.oid);

        /* Write max OID */
        if(H5_daos_write_max_oid(item->file) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't write max OID")

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, DAOS_OO_RW, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %d", ret)

        /* Encode datatype */
        if(H5Tencode(type_id, NULL, &type_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of datatype")
        if(NULL == (type_buf = DV_malloc(type_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized datatype")
        if(H5Tencode(type_id, type_buf, &type_size) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize datatype")

        /* Encode dataspace */
        if(H5Sencode(space_id, NULL, &space_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of dataaspace")
        if(NULL == (space_buf = DV_malloc(space_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized dataaspace")
        if(H5Sencode(space_id, space_buf, &space_size) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize dataaspace")

        /* Encode DCPL */
        if(H5Pencode(dcpl_id, NULL, &dcpl_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of dcpl")
        if(NULL == (dcpl_buf = DV_malloc(dcpl_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized dcpl")
        if(H5Pencode(dcpl_id, dcpl_buf, &dcpl_size) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize dcpl")

        /* Set up operation to write datatype, dataspace, and DCPL to dataset */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, (void *)type_key, (daos_size_t)(sizeof(type_key) - 1));
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = (uint64_t)type_size;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, (void *)space_key, (daos_size_t)(sizeof(space_key) - 1));
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = (uint64_t)space_size;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[2].iod_name, (void *)dcpl_key, (daos_size_t)(sizeof(dcpl_key) - 1));
        daos_csum_set(&iod[2].iod_kcsum, NULL, 0);
        iod[2].iod_nr = 1u;
        iod[2].iod_size = (uint64_t)dcpl_size;
        iod[2].iod_type = DAOS_IOD_SINGLE;

        /* Set up sgl */
        daos_iov_set(&sg_iov[0], type_buf, (daos_size_t)type_size);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        daos_iov_set(&sg_iov[1], space_buf, (daos_size_t)space_size);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];
        daos_iov_set(&sg_iov[2], dcpl_buf, (daos_size_t)dcpl_size);
        sgl[2].sg_nr = 1;
        sgl[2].sg_nr_out = 0;
        sgl[2].sg_iovs = &sg_iov[2];

        /* Write internal metadata to dataset */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 3, iod, sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't write metadata to dataset: %d", ret)

        /* Create link to dataset */
        if(name) {
            link_val.type = H5L_TYPE_HARD;
            link_val.target.hard = dset->obj.oid;
            if(H5_daos_link_write(target_grp, target_name, strlen(target_name), &link_val) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't create link to dataset")
        } /* end if */
    } /* end if */
    else {
        /* Update max_oid */
        item->file->max_oid = dset->obj.oid.lo;

        /* Note no barrier is currently needed here, daos_obj_open is a local
         * operation and can occur before the lead process writes metadata.  For
         * app-level synchronization we could add a barrier or bcast though it
         * could only be an issue with dataset reopen so we'll skip it for now.
         * There is probably never an issue with file reopen since all commits
         * are from process 0, same as the dataset create above. */

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, DAOS_OO_RW, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %d", ret)
    } /* end else */

    /* Finish setting up dataset struct */
    if((dset->type_id = H5Tcopy(type_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy datatype")
    if((dset->space_id = H5Scopy(space_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy dataspace")
    if(H5Sselect_all(dset->space_id) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTDELETE, NULL, "can't change selection")
    if((dset->dcpl_id = H5Pcopy(dcpl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy dcpl")
    if((dset->dapl_id = H5Pcopy(dapl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy dapl")

    /* Set return value */
    ret_value = (void *)dset;

done:
    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close group")

    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close dataset */
        if(dset && H5_daos_dataset_close(dset, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close dataset")

    /* Free memory */
    type_buf = DV_free(type_buf);
    space_buf = DV_free(space_buf);
    dcpl_buf = DV_free(dcpl_buf);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_dataset_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_open
 *
 * Purpose:     Sends a request to DAOS to open a dataset
 *
 * Return:      Success:        dataset object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_dataset_open(void *_item,
    const H5VL_loc_params_t DV_ATTR_UNUSED *loc_params, const char *name,
    hid_t dapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_dset_t *dset = NULL;
    H5_daos_group_t *target_grp = NULL;
    const char *target_name = NULL;
    daos_key_t dkey;
    daos_iod_t iod[3];
    daos_sg_list_t sgl[3];
    daos_iov_t sg_iov[3];
    uint64_t type_len = 0;
    uint64_t space_len = 0;
    uint64_t dcpl_len = 0;
    uint64_t tot_len;
    uint8_t dinfo_buf_static[H5_DAOS_DINFO_BUF_SIZE];
    uint8_t *dinfo_buf_dyn = NULL;
    uint8_t *dinfo_buf = dinfo_buf_static;
    char int_md_key[] = H5_DAOS_INT_MD_KEY;
    char type_key[] = H5_DAOS_TYPE_KEY;
    char space_key[] = H5_DAOS_SPACE_KEY;
    char dcpl_key[] = H5_DAOS_CPL_KEY;
    uint8_t *p;
    hbool_t collective = item->file->collective;
    hbool_t must_bcast = FALSE;
    int ret;
    void *ret_value = NULL;
 
    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(dapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, NULL, "can't get collective access property")

    /* Allocate the dataset object that is returned to the user */
    if(NULL == (dset = H5FL_CALLOC(H5_daos_dset_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS dataset struct")
    dset->obj.item.type = H5I_DATASET;
    dset->obj.item.file = item->file;
    dset->obj.item.rc = 1;
    dset->obj.obj_oh = DAOS_HDL_INVAL;
    dset->type_id = FAIL;
    dset->space_id = FAIL;
    dset->dcpl_id = FAIL;
    dset->dapl_id = FAIL;

    /* Check if we're actually opening the group or just receiving the dataset
     * info from the leader */
    if(!collective || (item->file->my_rank == 0)) {
        if(collective && (item->file->num_procs > 1))
            must_bcast = TRUE;

        /* Check for open by address */
        if(H5VL_OBJECT_BY_ADDR == loc_params->type) {
            /* Generate oid from address */
            H5_daos_oid_generate(&dset->obj.oid, (uint64_t)loc_params->loc_data.loc_by_addr.addr, H5I_DATASET);
        } /* end if */
        else {
            /* Open using name parameter */
            /* Traverse the path */
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, NULL, "can't traverse path")

            /* Follow link to dataset */
            if(H5_daos_link_follow(target_grp, target_name, strlen(target_name), dxpl_id, req, &dset->obj.oid) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "can't follow link to dataset")
        } /* end else */

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %d", ret)

        /* Set up operation to read datatype, dataspace, and DCPL sizes from
         * dataset */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, (void *)type_key, (daos_size_t)(sizeof(type_key) - 1));
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = DAOS_REC_ANY;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, (void *)space_key, (daos_size_t)(sizeof(space_key) - 1));
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = DAOS_REC_ANY;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[2].iod_name, (void *)dcpl_key, (daos_size_t)(sizeof(dcpl_key) - 1));
        daos_csum_set(&iod[2].iod_kcsum, NULL, 0);
        iod[2].iod_nr = 1u;
        iod[2].iod_size = DAOS_REC_ANY;
        iod[2].iod_type = DAOS_IOD_SINGLE;

        /* Read internal metadata sizes from dataset */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 3, iod, NULL,
                      NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTDECODE, NULL, "can't read metadata sizes from dataset: %d", ret)

        /* Check for metadata not found */
        if((iod[0].iod_size == (uint64_t)0) || (iod[1].iod_size == (uint64_t)0)
                || (iod[2].iod_size == (uint64_t)0))
            D_GOTO_ERROR(H5E_DATASET, H5E_NOTFOUND, NULL, "internal metadata not found")

        /* Compute dataset info buffer size */
        type_len = iod[0].iod_size;
        space_len = iod[1].iod_size;
        dcpl_len = iod[2].iod_size;
        tot_len = type_len + space_len + dcpl_len;

        /* Allocate dataset info buffer if necessary */
        if((tot_len + (5 * sizeof(uint64_t))) > sizeof(dinfo_buf_static)) {
            if(NULL == (dinfo_buf_dyn = (uint8_t *)DV_malloc(tot_len + (5 * sizeof(uint64_t)))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate dataset info buffer")
            dinfo_buf = dinfo_buf_dyn;
        } /* end if */

        /* Set up sgl */
        p = dinfo_buf + (5 * sizeof(uint64_t));
        daos_iov_set(&sg_iov[0], p, (daos_size_t)type_len);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        p += type_len;
        daos_iov_set(&sg_iov[1], p, (daos_size_t)space_len);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];
        p += space_len;
        daos_iov_set(&sg_iov[2], p, (daos_size_t)dcpl_len);
        sgl[2].sg_nr = 1;
        sgl[2].sg_nr_out = 0;
        sgl[2].sg_iovs = &sg_iov[2];

        /* Read internal metadata from dataset */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 3, iod, sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTDECODE, NULL, "can't read metadata from dataset: %d", ret)

        /* Broadcast dataset info if there are other processes that need it */
        if(collective && (item->file->num_procs > 1)) {
            assert(dinfo_buf);
            assert(sizeof(dinfo_buf_static) >= 5 * sizeof(uint64_t));

            /* Encode oid */
            p = dinfo_buf;
            UINT64ENCODE(p, dset->obj.oid.lo)
            UINT64ENCODE(p, dset->obj.oid.hi)

            /* Encode serialized info lengths */
            UINT64ENCODE(p, type_len)
            UINT64ENCODE(p, space_len)
            UINT64ENCODE(p, dcpl_len)

            /* MPI_Bcast dinfo_buf */
            if(MPI_SUCCESS != MPI_Bcast((char *)dinfo_buf, sizeof(dinfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't bcast dataset info")

            /* Need a second bcast if it did not fit in the receivers' static
             * buffer */
            if(tot_len + (5 * sizeof(uint64_t)) > sizeof(dinfo_buf_static))
                if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                    D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't bcast dataset info (second bcast)")
        } /* end if */
        else
            p = dinfo_buf + (5 * sizeof(uint64_t));
    } /* end if */
    else {
        /* Receive dataset info */
        if(MPI_SUCCESS != MPI_Bcast((char *)dinfo_buf, sizeof(dinfo_buf_static), MPI_BYTE, 0, item->file->comm))
            D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't bcast dataset info")

        /* Decode oid */
        p = dinfo_buf_static;
        UINT64DECODE(p, dset->obj.oid.lo)
        UINT64DECODE(p, dset->obj.oid.hi)

        /* Decode serialized info lengths */
        UINT64DECODE(p, type_len)
        UINT64DECODE(p, space_len)
        UINT64DECODE(p, dcpl_len)
        tot_len = type_len + space_len + dcpl_len;

        /* Check for type_len set to 0 - indicates failure */
        if(type_len == 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, NULL, "lead process failed to open dataset")

        /* Check if we need to perform another bcast */
        if(tot_len + (5 * sizeof(uint64_t)) > sizeof(dinfo_buf_static)) {
            /* Allocate a dynamic buffer if necessary */
            if(tot_len > sizeof(dinfo_buf_static)) {
                if(NULL == (dinfo_buf_dyn = (uint8_t *)DV_malloc(tot_len)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate space for dataset info")
                dinfo_buf = dinfo_buf_dyn;
            } /* end if */

            /* Receive dataset info */
            if(MPI_SUCCESS != MPI_Bcast((char *)dinfo_buf, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't bcast dataset info (second bcast)")

            p = dinfo_buf;
        } /* end if */

        /* Open dataset */
        if(0 != (ret = daos_obj_open(item->file->coh, dset->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &dset->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open dataset: %d", ret)
    } /* end else */

    /* Decode datatype, dataspace, and DCPL */
    if((dset->type_id = H5Tdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")
    p += type_len;
    if((dset->space_id = H5Sdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")
    if(H5Sselect_all(dset->space_id) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTDELETE, NULL, "can't change selection")
    p += space_len;
    if((dset->dcpl_id = H5Pdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize dataset creation property list")

    /* Finish setting up dataset struct */
    if((dset->dapl_id = H5Pcopy(dapl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy dapl");

    /* Set return value */
    ret_value = (void *)dset;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast dinfo_buf as '0' if necessary - this will trigger failures in
         * in other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(dinfo_buf_static, 0, sizeof(dinfo_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(dinfo_buf_static, sizeof(dinfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_DONE_ERROR(H5E_DATASET, H5E_MPI, NULL, "can't bcast empty dataset info")
        } /* end if */

        /* Close dataset */
        if(dset && H5_daos_dataset_close(dset, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close dataset")
    } /* end if */

    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_DATASET, H5E_CLOSEERROR, NULL, "can't close group")

    /* Free memory */
    dinfo_buf_dyn = (uint8_t *)DV_free(dinfo_buf_dyn);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_dataset_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_sel_to_recx_iov
 *
 * Purpose:     Given a dataspace with a selection and the datatype
 *              (element) size, build a list of DAOS records (recxs)
 *              and/or scatter/gather list I/O vectors (sg_iovs). *recxs
 *              and *sg_iovs should, if requested, point to a (probably
 *              statically allocated) single element.  Does not release
 *              buffers on error.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              December, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_sel_to_recx_iov(hid_t space_id, size_t type_size, void *buf,
    daos_recx_t **recxs, daos_iov_t **sg_iovs, size_t *list_nused)
{
    H5S_sel_iter_t *sel_iter = NULL;
    hbool_t sel_iter_init = FALSE;      /* Selection iteration info has been initialized */
    size_t nseq;
    size_t nelem;
    hsize_t off[H5_DAOS_SEQ_LIST_LEN];
    size_t len[H5_DAOS_SEQ_LIST_LEN];
    size_t buf_len = 1;
    void *vp_ret;
    size_t szi;
    herr_t ret_value = SUCCEED;

    assert(recxs || sg_iovs);
    assert(!recxs || *recxs);
    assert(!sg_iovs || *sg_iovs);
    assert(list_nused);

    /* Initialize list_nused */
    *list_nused = 0;

    /* Initialize selection iterator  */
    if(NULL == (sel_iter = H5Sselect_iter_init(space_id, (size_t)1)))
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTINIT, FAIL, "unable to initialize selection iterator")
    sel_iter_init = TRUE;       /* Selection iteration info has been initialized */

    /* Generate sequences from the file space until finished */
    do {
        /* Get the sequences of bytes */
        if(H5Sselect_get_seq_list(space_id, 0, sel_iter, (size_t)H5_DAOS_SEQ_LIST_LEN, (size_t)-1, &nseq, &nelem, off, len) < 0)
            D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "sequence length generation failed")

        /* Make room for sequences in recxs */
        if((buf_len == 1) && (nseq > 1)) {
            if(recxs)
                if(NULL == (*recxs = (daos_recx_t *)DV_malloc(H5_DAOS_SEQ_LIST_LEN * sizeof(daos_recx_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate memory for records")
            if(sg_iovs)
                if(NULL == (*sg_iovs = (daos_iov_t *)DV_malloc(H5_DAOS_SEQ_LIST_LEN * sizeof(daos_iov_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate memory for sgl iovs")
            buf_len = H5_DAOS_SEQ_LIST_LEN;
        } /* end if */
        else if(*list_nused + nseq > buf_len) {
            if(recxs) {
                if(NULL == (vp_ret = DV_realloc(*recxs, 2 * buf_len * sizeof(daos_recx_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't reallocate memory for records")
                *recxs = (daos_recx_t *)vp_ret;
            } /* end if */
            if(sg_iovs) {
                if(NULL == (vp_ret = DV_realloc(*sg_iovs, 2 * buf_len * sizeof(daos_iov_t))))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't reallocate memory for sgls")
                *sg_iovs = (daos_iov_t *)vp_ret;
            } /* end if */
            buf_len *= 2;
        } /* end if */
        assert(*list_nused + nseq <= buf_len);

        /* Copy offsets/lengths to recxs and sg_iovs */
        for(szi = 0; szi < nseq; szi++) {
            if(recxs) {
                (*recxs)[szi + *list_nused].rx_idx = (uint64_t)off[szi];
                (*recxs)[szi + *list_nused].rx_nr = (uint64_t)len[szi];
            } /* end if */
            if(sg_iovs)
                daos_iov_set(&(*sg_iovs)[szi + *list_nused],
                        (uint8_t *)buf + (off[szi] * type_size),
                        (daos_size_t)len[szi] * (daos_size_t)type_size);
        } /* end for */
        *list_nused += nseq;
    } while(nseq == H5_DAOS_SEQ_LIST_LEN);

done:
    /* Release selection iterator */
    if(sel_iter_init && H5Sselect_iter_release(sel_iter) < 0)
        D_DONE_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to release selection iterator")

    D_FUNC_LEAVE
} /* end H5_daos_sel_to_recx_iov() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_scatter_cb
 *
 * Purpose:     Callback function for H5Dscatter.  Simply passes the
 *              entire buffer described by udata to H5Dscatter.
 *
 * Return:      SUCCEED (never fails)
 *
 * Programmer:  Neil Fortner
 *              March, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_scatter_cb(const void **src_buf, size_t *src_buf_bytes_used,
    void *_udata)
{
    H5_daos_scatter_cb_ud_t *udata = (H5_daos_scatter_cb_ud_t *)_udata;
    herr_t ret_value = SUCCEED;

    /* Set src_buf and src_buf_bytes_used to use the entire buffer */
    *src_buf = udata->buf;
    *src_buf_bytes_used = udata->len;

    /* DSINC - This function used to always return SUCCEED without needing an
     * herr_t. Might need an additional FUNC_LEAVE macro to do this, or modify
     * the current one to take in the ret_value.
     */
    D_FUNC_LEAVE
} /* end H5_daos_scatter_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_mem_vl_rd_cb
 *
 * Purpose:     H5Diterate callback for iterating over the memory space
 *              before reading vl data.  Allocates vl read buffers,
 *              up scatter gather lists (sgls), and reshapes iods if
 *              necessary to skip empty elements.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_mem_vl_rd_cb(void *_elem, hid_t DV_ATTR_UNUSED type_id,
    unsigned DV_ATTR_UNUSED ndim, const hsize_t DV_ATTR_UNUSED *point,
    void *_udata)
{
    H5_daos_vl_mem_ud_t *udata = (H5_daos_vl_mem_ud_t *)_udata;
    herr_t ret_value = SUCCEED;

    /* Set up constant sgl info */
    udata->sgls[udata->idx].sg_nr = 1;
    udata->sgls[udata->idx].sg_nr_out = 0;
    udata->sgls[udata->idx].sg_iovs = &udata->sg_iovs[udata->idx];

    /* Check for empty element */
    if(udata->iods[udata->idx].iod_size == 0) {
        /* Increment offset, slide down following elements */
        udata->offset++;

        /* Zero out read buffer */
        if(udata->is_vl_str)
            *(char **)_elem = NULL;
        else
            memset(_elem, 0, sizeof(hvl_t));
    } /* end if */
    else {
        assert(udata->idx >= udata->offset);

        /* Check for vlen string */
        if(udata->is_vl_str) {
            char *elem = NULL;

            /* Allocate buffer for this vl element */
            if(NULL == (elem = (char *)malloc((size_t)udata->iods[udata->idx].iod_size + 1)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate vl data buffer")
            *(char **)_elem = elem;

            /* Add null terminator */
            elem[udata->iods[udata->idx].iod_size] = '\0';

            /* Set buffer location in sgl */
            daos_iov_set(&udata->sg_iovs[udata->idx - udata->offset], elem, udata->iods[udata->idx].iod_size);
        } /* end if */
        else {
            /* Standard vlen, find hvl_t struct for this element */
            hvl_t *elem = (hvl_t *)_elem;

            assert(udata->base_type_size > 0);

            /* Allocate buffer for this vl element and set size */
            elem->len = (size_t)udata->iods[udata->idx].iod_size / udata->base_type_size;
            if(NULL == (elem->p = malloc((size_t)udata->iods[udata->idx].iod_size)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate vl data buffer")

            /* Set buffer location in sgl */
            daos_iov_set(&udata->sg_iovs[udata->idx - udata->offset], elem->p, udata->iods[udata->idx].iod_size);
        } /* end if */

        /* Slide down iod if necessary */
        if(udata->offset)
            udata->iods[udata->idx - udata->offset] = udata->iods[udata->idx];
    } /* end else */

    /* Advance idx */
    udata->idx++;

done:
    D_FUNC_LEAVE
} /* end H5_daos_dataset_mem_vl_rd_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_file_vl_cb
 *
 * Purpose:     H5Diterate callback for iterating over the file space
 *              before vl data I/O.  Sets up akeys and iods (except for
 *              iod record sizes).
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_file_vl_cb(void DV_ATTR_UNUSED *_elem,
    hid_t DV_ATTR_UNUSED type_id, unsigned ndim, const hsize_t *point,
    void *_udata)
{
    H5_daos_vl_file_ud_t *udata = (H5_daos_vl_file_ud_t *)_udata;
    size_t akey_len = ndim * sizeof(uint64_t);
    uint64_t coordu64;
    uint8_t *p;
    unsigned i;
    herr_t ret_value = SUCCEED;

    /* Create akey for this element */
    if(NULL == (udata->akeys[udata->idx] = (uint8_t *)DV_malloc(akey_len)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey")
    p = udata->akeys[udata->idx];
    for(i = 0; i < ndim; i++) {
        coordu64 = (uint64_t)point[i];
        UINT64ENCODE(p, coordu64)
    } /* end for */

    /* Set up iod, size was set in memory callback or initialized in main read
     * function.  Use "single" records of varying size. */
    daos_iov_set(&udata->iods[udata->idx].iod_name, (void *)udata->akeys[udata->idx], (daos_size_t)akey_len);
    daos_csum_set(&udata->iods[udata->idx].iod_kcsum, NULL, 0);
    udata->iods[udata->idx].iod_nr = 1u;
    udata->iods[udata->idx].iod_type = DAOS_IOD_SINGLE;

    /* Advance idx */
    udata->idx++;

done:
    D_FUNC_LEAVE
} /* end H5_daos_dataset_file_vl_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_read
 *
 * Purpose:     Reads raw data from a dataset into a buffer.
 *`
 * Return:      Success:        0
 *              Failure:        -1, dataset not read.
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_read(void *_dset, hid_t mem_type_id, hid_t mem_space_id,
    hid_t file_space_id, hid_t dxpl_id, void *buf, void DV_ATTR_UNUSED **req)
{
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    H5S_sel_iter_t *sel_iter = NULL;
    hbool_t sel_iter_init = FALSE;      /* Selection iteration info has been initialized */
    int ndims;
    hsize_t dim[H5S_MAX_RANK];
    hid_t real_file_space_id;
    hid_t real_mem_space_id;
    hssize_t num_elem = -1;
    uint64_t chunk_coords[H5S_MAX_RANK];
    daos_key_t dkey;
    uint8_t **akeys = NULL;
    daos_iod_t *iods = NULL;
    daos_sg_list_t *sgls = NULL;
    daos_recx_t recx;
    daos_recx_t *recxs = &recx;
    daos_iov_t sg_iov;
    daos_iov_t *sg_iovs = &sg_iov;
    uint8_t dkey_buf[1 + H5S_MAX_RANK];
    hid_t base_type_id = FAIL;
    size_t base_type_size = 0;
    void *tconv_buf = NULL;
    void *bkg_buf = NULL;
    H5T_class_t type_class;
    hbool_t is_vl = FALSE;
    htri_t is_vl_str = FALSE;
    H5_daos_tconv_reuse_t reuse = H5_DAOS_TCONV_REUSE_NONE;
    uint8_t *p;
    int ret;
    uint64_t i;
    herr_t ret_value = SUCCEED;

    if(!buf)
        D_GOTO_ERROR(H5E_DATASET, H5E_BADVALUE, FAIL, "read buffer is NULL")

    /* Get dataspace extent */
    if((ndims = H5Sget_simple_extent_ndims(dset->space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of dimensions")
    if(ndims != H5Sget_simple_extent_dims(dset->space_id, dim, NULL))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dimensions")

    /* Get "real" space ids */
    if(file_space_id == H5S_ALL)
        real_file_space_id = dset->space_id;
    else
        real_file_space_id = file_space_id;
    if(mem_space_id == H5S_ALL)
        real_mem_space_id = real_file_space_id;
    else
        real_mem_space_id = mem_space_id;

    /* Encode dkey (chunk coordinates).  Prefix with '\0' to avoid accidental
     * collisions with other d-keys in this object.  For now just 1 chunk,
     * starting at 0. */
    memset(chunk_coords, 0, sizeof(chunk_coords)); /*DSINC*/
    p = dkey_buf;
    *p++ = (uint8_t)'\0';
    for(i = 0; i < (uint64_t)ndims; i++)
        UINT64ENCODE(p, chunk_coords[i])

    /* Set up dkey */
    daos_iov_set(&dkey, dkey_buf, (daos_size_t)(1 + ((size_t)ndims * sizeof(chunk_coords[0]))));

    /* Check for vlen */
    if(H5T_NO_CLASS == (type_class = H5Tget_class(mem_type_id)))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype class")
    if(type_class == H5T_VLEN) {
        is_vl = TRUE;

        /* Calculate base type size */
        if((base_type_id = H5Tget_super(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype base type")
        if(0 == (base_type_size = H5Tget_size(base_type_id)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype base type size")
    } /* end if */
    else if(type_class == H5T_STRING) {
        /* check for vlen string */
        if((is_vl_str = H5Tis_variable_str(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't check for variable length string")
        if(is_vl_str)
            is_vl = TRUE;
    } /* end if */

    /* Check for variable length */
    if(is_vl) {
        H5_daos_vl_mem_ud_t mem_ud;
        H5_daos_vl_file_ud_t file_ud;

        /* Get number of elements in selection */
        if((num_elem = H5Sget_select_npoints(real_mem_space_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of points in selection")

        /* Allocate array of akey pointers */
        if(NULL == (akeys = (uint8_t **)DV_calloc((size_t)num_elem * sizeof(uint8_t *))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey array")

        /* Allocate array of iods */
        if(NULL == (iods = (daos_iod_t *)DV_calloc((size_t)num_elem * sizeof(daos_iod_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for I/O descriptor array")

        /* Fill in size fields of iod as DAOS_REC_ANY so we can read the vl
         * sizes */
        for(i = 0; i < (uint64_t)num_elem; i++)
            iods[i].iod_size = DAOS_REC_ANY;

        /* Iterate over file selection.  Note the bogus buffer and type_id,
         * these don't matter since the "elem" parameter of the callback is not
         * used. */
        file_ud.akeys = akeys;
        file_ud.iods = iods;
        file_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, real_file_space_id, H5_daos_dataset_file_vl_cb, &file_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "file selection iteration failed")
        assert(file_ud.idx == (uint64_t)num_elem);

        /* Read vl sizes from dataset */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, (unsigned)num_elem, iods, NULL, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_ATTR, H5E_READERROR, FAIL, "can't read vl data sizes from dataset: %d", ret)

        /* Allocate array of sg_iovs */
        if(NULL == (sg_iovs = (daos_iov_t *)DV_malloc((size_t)num_elem * sizeof(daos_iov_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list")

        /* Allocate array of sgls */
        if(NULL == (sgls = (daos_sg_list_t *)DV_malloc((size_t)num_elem * sizeof(daos_sg_list_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list array")

        /* Iterate over memory selection */
        mem_ud.iods = iods;
        mem_ud.sgls = sgls;
        mem_ud.sg_iovs = sg_iovs;
        mem_ud.is_vl_str = is_vl_str;
        mem_ud.base_type_size = base_type_size;
        mem_ud.offset = 0;
        mem_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, real_mem_space_id, H5_daos_dataset_mem_vl_rd_cb, &mem_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "memory selection iteration failed")
        assert(mem_ud.idx == (uint64_t)num_elem);

        /* Read data from dataset */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, (unsigned)((uint64_t)num_elem - mem_ud.offset), iods, sgls, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from dataset: %d", ret)
    } /* end if */
    else {
        daos_iod_t iod;
        daos_sg_list_t sgl;
        uint8_t akey = H5_DAOS_CHUNK_KEY;
        size_t tot_nseq;
        size_t file_type_size;
        htri_t types_equal;

        /* Get datatype size */
        if((file_type_size = H5Tget_size(dset->type_id)) == 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype size")

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)&akey, (daos_size_t)(sizeof(akey)));
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_size = file_type_size;
        iod.iod_type = DAOS_IOD_ARRAY;

        /* Check if the types are equal */
        if((types_equal = H5Tequal(dset->type_id, mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTCOMPARE, FAIL, "can't check if types are equal")
        if(types_equal) {
            /* No type conversion necessary */
            /* Check for memory space is H5S_ALL, use file space in this case */
            if(mem_space_id == H5S_ALL) {
                /* Calculate both recxs and sg_iovs at the same time from file space */
                if(H5_daos_sel_to_recx_iov(real_file_space_id, file_type_size, buf, &recxs, &sg_iovs, &tot_nseq) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
                iod.iod_nr = (unsigned)tot_nseq;
                sgl.sg_nr = (uint32_t)tot_nseq;
                sgl.sg_nr_out = 0;
            } /* end if */
            else {
                /* Calculate recxs from file space */
                if(H5_daos_sel_to_recx_iov(real_file_space_id, file_type_size, buf, &recxs, NULL, &tot_nseq) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
                iod.iod_nr = (unsigned)tot_nseq;

                /* Calculate sg_iovs from mem space */
                if(H5_daos_sel_to_recx_iov(real_mem_space_id, file_type_size, buf, NULL, &sg_iovs, &tot_nseq) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
                sgl.sg_nr = (uint32_t)tot_nseq;
                sgl.sg_nr_out = 0;
            } /* end else */

            /* Point iod and sgl to lists generated above */
            iod.iod_recxs = recxs;
            sgl.sg_iovs = sg_iovs;

            /* Read data from dataset */
            if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
                D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from dataset: %d", ret)
        } /* end if */
        else {
            size_t nseq_tmp;
            size_t nelem_tmp;
            hsize_t sel_off;
            size_t sel_len;
            size_t mem_type_size;
            hbool_t fill_bkg = FALSE;
            hbool_t contig;

            /* Type conversion necessary */
            /* Get number of elements in selection */
            if((num_elem = H5Sget_select_npoints(real_mem_space_id)) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of points in selection")

            /* Calculate recxs from file space */
            if(H5_daos_sel_to_recx_iov(real_file_space_id, file_type_size, buf, &recxs, NULL, &tot_nseq) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
            iod.iod_nr = (unsigned)tot_nseq;
            iod.iod_recxs = recxs;

            /* Set up constant sgl info */
            sgl.sg_nr = 1;
            sgl.sg_nr_out = 0;
            sgl.sg_iovs = &sg_iov;

            /* Check for contiguous memory buffer */

            /* Initialize selection iterator  */
            if(NULL == (sel_iter = H5Sselect_iter_init(real_mem_space_id, (size_t)1)))
                D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTINIT, FAIL, "unable to initialize selection iterator")
            sel_iter_init = TRUE;       /* Selection iteration info has been initialized */

            /* Get the sequence list - only check the first sequence because we only
             * care if it is contiguous and if so where the contiguous selection
             * begins */
            if(H5Sselect_get_seq_list(real_mem_space_id, 0, sel_iter, (size_t)1, (size_t)-1, &nseq_tmp, &nelem_tmp, &sel_off, &sel_len) < 0)
                D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTGET, FAIL, "sequence length generation failed")
            contig = (sel_len == (size_t)num_elem);

            /* Initialize type conversion */
            if(H5_daos_tconv_init(dset->type_id, &file_type_size, mem_type_id, &mem_type_size, (size_t)num_elem, &tconv_buf, &bkg_buf, contig ? &reuse : NULL, &fill_bkg) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't initialize type conversion")

            /* Reuse buffer as appropriate */
            if(contig) {
                sel_off *= (hsize_t)mem_type_size;
                if(reuse == H5_DAOS_TCONV_REUSE_TCONV)
                    tconv_buf = (char *)buf + (size_t)sel_off;
                else if(reuse == H5_DAOS_TCONV_REUSE_BKG)
                    bkg_buf = (char *)buf + (size_t)sel_off;
            } /* end if */

            /* Set sg_iov to point to tconv_buf */
            daos_iov_set(&sg_iov, tconv_buf, (daos_size_t)num_elem * (daos_size_t)file_type_size);

            /* Read data to tconv_buf */
            if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
                D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from attribute: %d", ret)

            /* Gather data to background buffer if necessary */
            if(fill_bkg && (reuse != H5_DAOS_TCONV_REUSE_BKG))
                if(H5Dgather(real_mem_space_id, buf, mem_type_id, (size_t)num_elem * mem_type_size, bkg_buf, NULL, NULL) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't gather data to background buffer")

            /* Perform type conversion */
            if(H5Tconvert(dset->type_id, mem_type_id, (size_t)num_elem, tconv_buf, bkg_buf, dxpl_id) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTCONVERT, FAIL, "can't perform type conversion")

            /* Scatter data to memory buffer if necessary */
            if(reuse != H5_DAOS_TCONV_REUSE_TCONV) {
                H5_daos_scatter_cb_ud_t scatter_cb_ud;

                scatter_cb_ud.buf = tconv_buf;
                scatter_cb_ud.len = (size_t)num_elem * mem_type_size;
                if(H5Dscatter(H5_daos_scatter_cb, &scatter_cb_ud, mem_type_id, real_mem_space_id, buf) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't scatter data to read buffer")
            } /* end if */
        } /* end else */
    } /* end else */

done:
    /* Free memory */
    iods = (daos_iod_t *)DV_free(iods);
    if(recxs != &recx)
        DV_free(recxs);
    sgls = (daos_sg_list_t *)DV_free(sgls);
    if(sg_iovs != &sg_iov)
        DV_free(sg_iovs);
    if(tconv_buf && (reuse != H5_DAOS_TCONV_REUSE_TCONV))
        DV_free(tconv_buf);
    if(bkg_buf && (reuse != H5_DAOS_TCONV_REUSE_BKG))
        DV_free(bkg_buf);

    if(akeys) {
        for(i = 0; i < (uint64_t)num_elem; i++)
            DV_free(akeys[i]);
        DV_free(akeys);
    } /* end if */

    if(base_type_id != FAIL)
        if(H5Idec_ref(base_type_id) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close base type id")

    /* Release selection iterator */
    if(sel_iter_init && H5Sselect_iter_release(sel_iter) < 0)
        D_DONE_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to release selection iterator")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_dataset_read() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_mem_vl_wr_cb
 *
 * Purpose:     H5Diterate callback for iterating over the memory space
 *              before writing vl data.  Sets up scatter gather lists
 *              (sgls) and sets the record sizes in iods.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_mem_vl_wr_cb(void *_elem, hid_t DV_ATTR_UNUSED type_id,
    unsigned DV_ATTR_UNUSED ndim, const hsize_t DV_ATTR_UNUSED *point,
    void *_udata)
{
    H5_daos_vl_mem_ud_t *udata = (H5_daos_vl_mem_ud_t *)_udata;
    herr_t ret_value = SUCCEED;

    /* Set up constant sgl info */
    udata->sgls[udata->idx].sg_nr = 1;
    udata->sgls[udata->idx].sg_nr_out = 0;
    udata->sgls[udata->idx].sg_iovs = &udata->sg_iovs[udata->idx];

    /* Check for vlen string */
    if(udata->is_vl_str) {
        /* Find string for this element */
        char *elem = *(char **)_elem;

        /* Set string length in iod and buffer location in sgl.  If we are
         * writing an empty string ("\0"), increase the size by one to
         * differentiate it from NULL strings.  Note that this will cause the
         * read buffer to be one byte longer than it needs to be in this case.
         * This should not cause any ill effects. */
        if(elem) {
            udata->iods[udata->idx].iod_size = (daos_size_t)strlen(elem);
            if(udata->iods[udata->idx].iod_size == 0)
                udata->iods[udata->idx].iod_size = 1;
            daos_iov_set(&udata->sg_iovs[udata->idx], (void *)elem, udata->iods[udata->idx].iod_size);
        } /* end if */
        else {
            udata->iods[udata->idx].iod_size = 0;
            daos_iov_set(&udata->sg_iovs[udata->idx], NULL, 0);
        } /* end else */
    } /* end if */
    else {
        /* Standard vlen, find hvl_t struct for this element */
        hvl_t *elem = (hvl_t *)_elem;

        assert(udata->base_type_size > 0);

        /* Set buffer length in iod and buffer location in sgl */
        if(elem->len > 0) {
            udata->iods[udata->idx].iod_size = (daos_size_t)(elem->len * udata->base_type_size);
            daos_iov_set(&udata->sg_iovs[udata->idx], (void *)elem->p, udata->iods[udata->idx].iod_size);
        } /* end if */
        else {
            udata->iods[udata->idx].iod_size = 0;
            daos_iov_set(&udata->sg_iovs[udata->idx], NULL, 0);
        } /* end else */
    } /* end else */

    /* Advance idx */
    udata->idx++;

    /* DSINC - This function used to always return SUCCEED without needing an
     * herr_t. Might need an additional FUNC_LEAVE macro to do this, or modify
     * the current one to take in the ret_value.
     */
    D_FUNC_LEAVE
} /* end H5_daos_dataset_mem_vl_wr_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_write
 *
 * Purpose:     Writes raw data from a buffer into a dataset.
 *
 * Return:      Success:        0
 *              Failure:        -1, dataset not written.
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_write(void *_dset, hid_t mem_type_id, hid_t mem_space_id,
    hid_t file_space_id, hid_t DV_ATTR_UNUSED dxpl_id,
    const void *buf, void DV_ATTR_UNUSED **req)
{
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    int ndims;
    hsize_t dim[H5S_MAX_RANK];
    hid_t real_file_space_id;
    hid_t real_mem_space_id;
    hssize_t num_elem;
    uint64_t chunk_coords[H5S_MAX_RANK];
    daos_key_t dkey;
    uint8_t **akeys = NULL;
    daos_iod_t *iods = NULL;
    daos_sg_list_t *sgls = NULL;
    daos_recx_t recx;
    daos_recx_t *recxs = &recx;
    daos_iov_t sg_iov;
    daos_iov_t *sg_iovs = &sg_iov;
    uint8_t dkey_buf[1 + H5S_MAX_RANK];
    hid_t base_type_id = FAIL;
    size_t base_type_size = 0;
    void *tconv_buf = NULL;
    void *bkg_buf = NULL;
    H5T_class_t type_class;
    hbool_t is_vl = FALSE;
    htri_t is_vl_str = FALSE;
    uint8_t *p;
    int ret;
    uint64_t i;
    herr_t ret_value = SUCCEED;

    if(!buf)
        D_GOTO_ERROR(H5E_DATASET, H5E_BADVALUE, FAIL, "write buffer is NULL")

    /* Check for write access */
    if(!(dset->obj.item.file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "no write intent on file")

    /* Get dataspace extent */
    if((ndims = H5Sget_simple_extent_ndims(dset->space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of dimensions")
    if(ndims != H5Sget_simple_extent_dims(dset->space_id, dim, NULL))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dimensions")

    /* Get "real" space ids */
    if(file_space_id == H5S_ALL)
        real_file_space_id = dset->space_id;
    else
        real_file_space_id = file_space_id;
    if(mem_space_id == H5S_ALL)
        real_mem_space_id = real_file_space_id;
    else
        real_mem_space_id = mem_space_id;

    /* Get number of elements in selection */
    if((num_elem = H5Sget_select_npoints(real_mem_space_id)) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get number of points in selection")

    /* Encode dkey (chunk coordinates).  Prefix with '\0' to avoid accidental
     * collisions with other d-keys in this object.  For now just 1 chunk,
     * starting at 0. */
    memset(chunk_coords, 0, sizeof(chunk_coords)); /*DSINC*/
    p = dkey_buf;
    *p++ = (uint8_t)'\0';
    for(i = 0; i < (uint64_t)ndims; i++)
        UINT64ENCODE(p, chunk_coords[i])

    /* Set up dkey */
    daos_iov_set(&dkey, dkey_buf, (daos_size_t)(1 + ((size_t)ndims * sizeof(chunk_coords[0]))));

    /* Check for vlen */
    if(H5T_NO_CLASS == (type_class = H5Tget_class(mem_type_id)))
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype class")
    if(type_class == H5T_VLEN) {
        is_vl = TRUE;

        /* Calculate base type size */
        if((base_type_id = H5Tget_super(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype base type")
        if(0 == (base_type_size = H5Tget_size(base_type_id)))
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype base type size")
    } /* end if */
    else if(type_class == H5T_STRING) {
        /* check for vlen string */
        if((is_vl_str = H5Tis_variable_str(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't check for variable length string")
        if(is_vl_str)
            is_vl = TRUE;
    } /* end if */

    /* Check for variable length */
    if(is_vl) {
        H5_daos_vl_mem_ud_t mem_ud;
        H5_daos_vl_file_ud_t file_ud;

        /* Allocate array of akey pointers */
        if(NULL == (akeys = (uint8_t **)DV_calloc((size_t)num_elem * sizeof(uint8_t *))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey array")

        /* Allocate array of iods */
        if(NULL == (iods = (daos_iod_t *)DV_calloc((size_t)num_elem * sizeof(daos_iod_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for I/O descriptor array")

        /* Allocate array of sg_iovs */
        if(NULL == (sg_iovs = (daos_iov_t *)DV_malloc((size_t)num_elem * sizeof(daos_iov_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list")

        /* Allocate array of sgls */
        if(NULL == (sgls = (daos_sg_list_t *)DV_malloc((size_t)num_elem * sizeof(daos_sg_list_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list array")

        /* Iterate over memory selection */
        mem_ud.iods = iods;
        mem_ud.sgls = sgls;
        mem_ud.sg_iovs = sg_iovs;
        mem_ud.is_vl_str = is_vl_str;
        mem_ud.base_type_size = base_type_size;
        mem_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, real_mem_space_id, H5_daos_dataset_mem_vl_wr_cb, &mem_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "memory selection iteration failed")
        assert(mem_ud.idx == (uint64_t)num_elem);

        /* Iterate over file selection.  Note the bogus buffer and type_id,
         * these don't matter since the "elem" parameter of the callback is not
         * used. */
        file_ud.akeys = akeys;
        file_ud.iods = iods;
        file_ud.idx = 0;
        if(H5Diterate((void *)buf, mem_type_id, real_file_space_id, H5_daos_dataset_file_vl_cb, &file_ud) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "file selection iteration failed")
        assert(file_ud.idx == (uint64_t)num_elem);

        /* Write data to dataset */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, (unsigned)num_elem, iods, sgls, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_WRITEERROR, FAIL, "can't write data to dataset: %d", ret)
    } /* end if */
    else {
        daos_iod_t iod;
        daos_sg_list_t sgl;
        uint8_t akey = H5_DAOS_CHUNK_KEY;
        size_t tot_nseq;
        size_t file_type_size;
        size_t mem_type_size;
        hbool_t fill_bkg = FALSE;

        /* Initialize type conversion */
        if(H5_daos_tconv_init(mem_type_id, &mem_type_size, dset->type_id, &file_type_size, (size_t)num_elem, &tconv_buf, &bkg_buf, NULL, &fill_bkg) < 0)
            D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't initialize type conversion")

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)&akey, (daos_size_t)(sizeof(akey)));
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_size = file_type_size;
        iod.iod_type = DAOS_IOD_ARRAY;

        /* Build recxs and sg_iovs */

        /* Check for type conversion */
        if(tconv_buf) {
            /* Calculate recxs from file space */
            if(H5_daos_sel_to_recx_iov(real_file_space_id, file_type_size, (void *)buf, &recxs, NULL, &tot_nseq) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
            iod.iod_nr = (unsigned)tot_nseq;
            iod.iod_recxs = recxs;

            /* Set up constant sgl info */
            sgl.sg_nr = 1;
            sgl.sg_nr_out = 0;
            sgl.sg_iovs = &sg_iov;

            /* Check if we need to fill background buffer */
            if(fill_bkg) {
                assert(bkg_buf);

                /* Set sg_iov to point to background buffer */
                daos_iov_set(&sg_iov, bkg_buf, (daos_size_t)num_elem * (daos_size_t)file_type_size);

                /* Read data from dataset to background buffer */
                if(0 != (ret = daos_obj_fetch(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
                    D_GOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't read data from dataset: %d", ret)

                /* Reset iod_size, if the dataset was not allocated then it could
                 * have been overwritten by daos_obj_fetch */
                iod.iod_size = file_type_size;
            } /* end if */

            /* Gather data to conversion buffer */
            if(H5Dgather(real_mem_space_id, buf, mem_type_id, (size_t)num_elem * mem_type_size, tconv_buf, NULL, NULL) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't gather data to conversion buffer")

            /* Perform type conversion */
            if(H5Tconvert(mem_type_id, dset->type_id, (size_t)num_elem, tconv_buf, bkg_buf, dxpl_id) < 0)
                D_GOTO_ERROR(H5E_DATASET, H5E_CANTCONVERT, FAIL, "can't perform type conversion")

            /* Set sg_iovs to write from tconv_buf */
            daos_iov_set(&sg_iov, tconv_buf, (daos_size_t)num_elem * (daos_size_t)file_type_size);
        } /* end if */
        else {
            /* Check for memory space is H5S_ALL, use file space in this case */
            if(mem_space_id == H5S_ALL) {
                /* Calculate both recxs and sg_iovs at the same time from file space */
                if(H5_daos_sel_to_recx_iov(real_file_space_id, file_type_size, (void *)buf, &recxs, &sg_iovs, &tot_nseq) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
                iod.iod_nr = (unsigned)tot_nseq;
                sgl.sg_nr = (uint32_t)tot_nseq;
                sgl.sg_nr_out = 0;
            } /* end if */
            else {
                /* Calculate recxs from file space */
                if(H5_daos_sel_to_recx_iov(real_file_space_id, file_type_size, (void *)buf, &recxs, NULL, &tot_nseq) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
                iod.iod_nr = (unsigned)tot_nseq;

                /* Calculate sg_iovs from mem space */
                if(H5_daos_sel_to_recx_iov(real_mem_space_id, file_type_size, (void *)buf, NULL, &sg_iovs, &tot_nseq) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't generate sequence lists for DAOS I/O")
                sgl.sg_nr = (uint32_t)tot_nseq;
                sgl.sg_nr_out = 0;
            } /* end else */

            /* Point iod and sgl to lists generated above */
            iod.iod_recxs = recxs;
            sgl.sg_iovs = sg_iovs;
        } /* end else */

        /* Write data to dataset */
        if(0 != (ret = daos_obj_update(dset->obj.obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATASET, H5E_WRITEERROR, FAIL, "can't write data to dataset: %d", ret)
    } /* end else */

done:
    /* Free memory */
    iods = (daos_iod_t *)DV_free(iods);
    if(recxs != &recx)
        DV_free(recxs);
    sgls = (daos_sg_list_t *)DV_free(sgls);
    if(sg_iovs && (sg_iovs != &sg_iov))
        DV_free(sg_iovs);
    tconv_buf = DV_free(tconv_buf);
    bkg_buf = DV_free(bkg_buf);

    if(akeys) {
        for(i = 0; i < (uint64_t)num_elem; i++)
            DV_free(akeys[i]);
        DV_free(akeys);
    } /* end if */

    if(base_type_id != FAIL)
        if(H5Idec_ref(base_type_id) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close base type id")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_dataset_write() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_get
 *
 * Purpose:     Gets certain information about a dataset
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_dataset_get(void *_dset, H5VL_dataset_get_t get_type,
    hid_t DV_ATTR_UNUSED dxpl_id, void DV_ATTR_UNUSED **req, va_list arguments)
{
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    herr_t       ret_value = SUCCEED;    /* Return value */

    switch (get_type) {
        case H5VL_DATASET_GET_DCPL:
            {
                hid_t *plist_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's creation property list */
                if((*plist_id = H5Pcopy(dset->dcpl_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dset creation property list")

                break;
            } /* end block */
        case H5VL_DATASET_GET_DAPL:
            {
                hid_t *plist_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's access property list */
                if((*plist_id = H5Pcopy(dset->dapl_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dset access property list")

                break;
            } /* end block */
        case H5VL_DATASET_GET_SPACE:
            {
                hid_t *ret_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's dataspace */
                if((*ret_id = H5Scopy(dset->space_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get dataspace ID of dataset");
                break;
            } /* end block */
        case H5VL_DATASET_GET_SPACE_STATUS:
            {
                H5D_space_status_t *allocation = va_arg(arguments, H5D_space_status_t *);

                /* Retrieve the dataset's space status */
                *allocation = H5D_SPACE_STATUS_NOT_ALLOCATED;
                break;
            } /* end block */
        case H5VL_DATASET_GET_TYPE:
            {
                hid_t *ret_id = va_arg(arguments, hid_t *);

                /* Retrieve the dataset's datatype */
                if((*ret_id = H5Tcopy(dset->type_id)) < 0)
                    D_GOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get datatype ID of dataset")
                break;
            } /* end block */
        case H5VL_DATASET_GET_STORAGE_SIZE:
        case H5VL_DATASET_GET_OFFSET:
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "can't get this type of information from dataset")
    } /* end switch */

done:
    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_dataset_get() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_dataset_close
 *
 * Purpose:     Closes a DAOS HDF5 dataset.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_dataset_close(void *_dset, hid_t DV_ATTR_UNUSED dxpl_id,
    void DV_ATTR_UNUSED **req)
{
    H5_daos_dset_t *dset = (H5_daos_dset_t *)_dset;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(dset);

    if(--dset->obj.item.rc == 0) {
        /* Free dataset data structures */
        if(!daos_handle_is_inval(dset->obj.obj_oh))
            if(0 != (ret = daos_obj_close(dset->obj.obj_oh, NULL /*event*/)))
                D_DONE_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, FAIL, "can't close dataset DAOS object: %d", ret)
        if(dset->type_id != FAIL && H5Idec_ref(dset->type_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close datatype")
        if(dset->space_id != FAIL && H5Idec_ref(dset->space_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close dataspace")
        if(dset->dcpl_id != FAIL && H5Idec_ref(dset->dcpl_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close plist")
        if(dset->dapl_id != FAIL && H5Idec_ref(dset->dapl_id) < 0)
            D_DONE_ERROR(H5E_DATASET, H5E_CANTDEC, FAIL, "failed to close plist")
        dset = H5FL_FREE(H5_daos_dset_t, dset);
    } /* end if */

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_dataset_close() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_datatype_commit
 *
 * Purpose:     Commits a datatype inside the container.
 *
 * Return:      Success:        datatype ID.
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              June, 2017
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_datatype_commit(void *_item,
    const H5VL_loc_params_t DV_ATTR_UNUSED *loc_params, const char *name,
    hid_t type_id, hid_t DV_ATTR_UNUSED lcpl_id, hid_t tcpl_id, hid_t tapl_id,
    hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_dtype_t *dtype = NULL;
    H5_daos_group_t *target_grp = NULL;
    void *type_buf = NULL;
    void *tcpl_buf = NULL;
    hbool_t collective = item->file->collective;
    int ret;
    void *ret_value = NULL;

    /* Check for write access */
    if(!(item->file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, NULL, "no write intent on file")

    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(tapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, NULL, "can't get collective access property")

    /* Allocate the dataset object that is returned to the user */
    if(NULL == (dtype = H5FL_CALLOC(H5_daos_dtype_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS dataset struct")
    dtype->obj.item.type = H5I_DATATYPE;
    dtype->obj.item.file = item->file;
    dtype->obj.item.rc = 1;
    dtype->obj.obj_oh = DAOS_HDL_INVAL;
    dtype->type_id = FAIL;
    dtype->tcpl_id = FAIL;
    dtype->tapl_id = FAIL;

    /* Generate datatype oid */
    H5_daos_oid_encode(&dtype->obj.oid, item->file->max_oid + (uint64_t)1, H5I_DATATYPE);

    /* Create datatype and write metadata if this process should */
    if(!collective || (item->file->my_rank == 0)) {
        const char *target_name = NULL;
        H5_daos_link_val_t link_val;
        daos_key_t dkey;
        daos_iod_t iod[2];
        daos_sg_list_t sgl[2];
        daos_iov_t sg_iov[2];
        size_t type_size = 0;
        size_t tcpl_size = 0;
        char int_md_key[] = H5_DAOS_INT_MD_KEY;
        char type_key[] = H5_DAOS_TYPE_KEY;
        char tcpl_key[] = H5_DAOS_CPL_KEY;

        /* Traverse the path */
        if(name)
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_DATATYPE, H5E_BADITER, NULL, "can't traverse path")

        /* Create datatype */
        /* Update max_oid */
        item->file->max_oid = H5_daos_oid_to_idx(dtype->obj.oid);

        /* Write max OID */
        if(H5_daos_write_max_oid(item->file) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, NULL, "can't write max OID")

        /* Open datatype */
        if(0 != (ret = daos_obj_open(item->file->coh, dtype->obj.oid, DAOS_OO_RW, &dtype->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTOPENOBJ, NULL, "can't open datatype: %d", ret)

        /* Encode datatype */
        if(H5Tencode(type_id, NULL, &type_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of datatype")
        if(NULL == (type_buf = DV_malloc(type_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized datatype")
        if(H5Tencode(type_id, type_buf, &type_size) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTENCODE, NULL, "can't serialize datatype")

        /* Encode TCPL */
        if(H5Pencode(tcpl_id, NULL, &tcpl_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of tcpl")
        if(NULL == (tcpl_buf = DV_malloc(tcpl_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized tcpl")
        if(H5Pencode(tcpl_id, tcpl_buf, &tcpl_size) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTENCODE, NULL, "can't serialize tcpl")

        /* Set up operation to write datatype and TCPL to datatype */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, (void *)type_key, (daos_size_t)(sizeof(type_key) - 1));
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = (uint64_t)type_size;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, (void *)tcpl_key, (daos_size_t)(sizeof(tcpl_key) - 1));
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = (uint64_t)tcpl_size;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        /* Set up sgl */
        daos_iov_set(&sg_iov[0], type_buf, (daos_size_t)type_size);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        daos_iov_set(&sg_iov[1], tcpl_buf, (daos_size_t)tcpl_size);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];

        /* Write internal metadata to datatype */
        if(0 != (ret = daos_obj_update(dtype->obj.obj_oh, DAOS_TX_NONE, &dkey, 2, iod, sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, NULL, "can't write metadata to datatype: %d", ret)

        /* Create link to datatype */
        if(name) {
            link_val.type = H5L_TYPE_HARD;
            link_val.target.hard = dtype->obj.oid;
            if(H5_daos_link_write(target_grp, target_name, strlen(target_name), &link_val) < 0)
                D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, NULL, "can't create link to datatype")
        } /* end if */
    } /* end if */
    else {
        /* Update max_oid */
        item->file->max_oid = dtype->obj.oid.lo;

        /* Note no barrier is currently needed here, daos_obj_open is a local
         * operation and can occur before the lead process writes metadata.  For
         * app-level synchronization we could add a barrier or bcast though it
         * could only be an issue with datatype reopen so we'll skip it for now.
         * There is probably never an issue with file reopen since all commits
         * are from process 0, same as the datatype create above. */

        /* Open datatype */
        if(0 != (ret = daos_obj_open(item->file->coh, dtype->obj.oid, DAOS_OO_RW, &dtype->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTOPENOBJ, NULL, "can't open datatype: %d", ret)
    } /* end else */

    /* Finish setting up datatype struct */
    if((dtype->type_id = H5Tcopy(type_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy datatype")
    if((dtype->tcpl_id = H5Pcopy(tcpl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy tcpl")
    if((dtype->tapl_id = H5Pcopy(tapl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy tapl")

    /* Set return value */
    ret_value = (void *)dtype;

done:
    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, NULL, "can't close group")

    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close dataset */
        if(dtype && H5_daos_datatype_close(dtype, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, NULL, "can't close datatype")

    /* Free memory */
    type_buf = DV_free(type_buf);
    tcpl_buf = DV_free(tcpl_buf);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_datatype_commit() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_datatype_open
 *
 * Purpose:     Opens a DAOS HDF5 datatype.
 *
 * Return:      Success:        datatype ID.
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              April, 2017
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_datatype_open(void *_item,
    const H5VL_loc_params_t DV_ATTR_UNUSED *loc_params, const char *name,
    hid_t tapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_dtype_t *dtype = NULL;
    H5_daos_group_t *target_grp = NULL;
    const char *target_name = NULL;
    daos_key_t dkey;
    daos_iod_t iod[2];
    daos_sg_list_t sgl[2];
    daos_iov_t sg_iov[2];
    uint64_t type_len = 0;
    uint64_t tcpl_len = 0;
    uint64_t tot_len;
    uint8_t tinfo_buf_static[H5_DAOS_TINFO_BUF_SIZE];
    uint8_t *tinfo_buf_dyn = NULL;
    uint8_t *tinfo_buf = tinfo_buf_static;
    char int_md_key[] = H5_DAOS_INT_MD_KEY;
    char type_key[] = H5_DAOS_TYPE_KEY;
    char tcpl_key[] = H5_DAOS_CPL_KEY;
    uint8_t *p;
    hbool_t collective = item->file->collective;
    hbool_t must_bcast = FALSE;
    int ret;
    void *ret_value = NULL;

    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(tapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, NULL, "can't get collective access property")

    /* Allocate the datatype object that is returned to the user */
    if(NULL == (dtype = H5FL_CALLOC(H5_daos_dtype_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS datatype struct")
    dtype->obj.item.type = H5I_DATATYPE;
    dtype->obj.item.file = item->file;
    dtype->obj.item.rc = 1;
    dtype->obj.obj_oh = DAOS_HDL_INVAL;
    dtype->type_id = FAIL;
    dtype->tcpl_id = FAIL;
    dtype->tapl_id = FAIL;

    /* Check if we're actually opening the group or just receiving the datatype
     * info from the leader */
    if(!collective || (item->file->my_rank == 0)) {
        if(collective && (item->file->num_procs > 1))
            must_bcast = TRUE;

        /* Check for open by address */
        if(H5VL_OBJECT_BY_ADDR == loc_params->type) {
            /* Generate oid from address */
            H5_daos_oid_generate(&dtype->obj.oid, (uint64_t)loc_params->loc_data.loc_by_addr.addr, H5I_DATATYPE);
        } /* end if */
        else {
            /* Open using name parameter */
            /* Traverse the path */
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_DATATYPE, H5E_BADITER, NULL, "can't traverse path")

            /* Follow link to datatype */
            if(H5_daos_link_follow(target_grp, target_name, strlen(target_name), dxpl_id, req, &dtype->obj.oid) < 0)
                D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, NULL, "can't follow link to datatype")
        } /* end else */

        /* Open datatype */
        if(0 != (ret = daos_obj_open(item->file->coh, dtype->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &dtype->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTOPENOBJ, NULL, "can't open datatype: %d", ret)

        /* Set up operation to read datatype and TCPL sizes from datatype */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, (void *)type_key, (daos_size_t)(sizeof(type_key) - 1));
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = DAOS_REC_ANY;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, (void *)tcpl_key, (daos_size_t)(sizeof(tcpl_key) - 1));
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = DAOS_REC_ANY;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        /* Read internal metadata sizes from datatype */
        if(0 != (ret = daos_obj_fetch(dtype->obj.obj_oh, DAOS_TX_NONE, &dkey, 2, iod, NULL,
                      NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTDECODE, NULL, "can't read metadata sizes from datatype: %d", ret)

        /* Check for metadata not found */
        if((iod[0].iod_size == (uint64_t)0) || (iod[1].iod_size == (uint64_t)0))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_NOTFOUND, NULL, "internal metadata not found")

        /* Compute datatype info buffer size */
        type_len = iod[0].iod_size;
        tcpl_len = iod[1].iod_size;
        tot_len = type_len + tcpl_len;

        /* Allocate datatype info buffer if necessary */
        if((tot_len + (4 * sizeof(uint64_t))) > sizeof(tinfo_buf_static)) {
            if(NULL == (tinfo_buf_dyn = (uint8_t *)DV_malloc(tot_len + (4 * sizeof(uint64_t)))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate datatype info buffer")
            tinfo_buf = tinfo_buf_dyn;
        } /* end if */

        /* Set up sgl */
        p = tinfo_buf + (4 * sizeof(uint64_t));
        daos_iov_set(&sg_iov[0], p, (daos_size_t)type_len);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        p += type_len;
        daos_iov_set(&sg_iov[1], p, (daos_size_t)tcpl_len);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];

        /* Read internal metadata from datatype */
        if(0 != (ret = daos_obj_fetch(dtype->obj.obj_oh, DAOS_TX_NONE, &dkey, 2, iod, sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTDECODE, NULL, "can't read metadata from datatype: %d", ret)

        /* Broadcast datatype info if there are other processes that need it */
        if(collective && (item->file->num_procs > 1)) {
            assert(tinfo_buf);
            assert(sizeof(tinfo_buf_static) >= 4 * sizeof(uint64_t));

            /* Encode oid */
            p = tinfo_buf;
            UINT64ENCODE(p, dtype->obj.oid.lo)
            UINT64ENCODE(p, dtype->obj.oid.hi)

            /* Encode serialized info lengths */
            UINT64ENCODE(p, type_len)
            UINT64ENCODE(p, tcpl_len)

            /* MPI_Bcast dinfo_buf */
            if(MPI_SUCCESS != MPI_Bcast((char *)tinfo_buf, sizeof(tinfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_DATATYPE, H5E_MPI, NULL, "can't bcast datatype info")

            /* Need a second bcast if it did not fit in the receivers' static
             * buffer */
            if(tot_len + (4 * sizeof(uint64_t)) > sizeof(tinfo_buf_static))
                if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                    D_GOTO_ERROR(H5E_DATATYPE, H5E_MPI, NULL, "can't bcast datatype info (second bcast)")
        } /* end if */
        else
            p = tinfo_buf + (4 * sizeof(uint64_t));
    } /* end if */
    else {
        /* Receive datatype info */
        if(MPI_SUCCESS != MPI_Bcast((char *)tinfo_buf, sizeof(tinfo_buf_static), MPI_BYTE, 0, item->file->comm))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_MPI, NULL, "can't bcast datatype info")

        /* Decode oid */
        p = tinfo_buf_static;
        UINT64DECODE(p, dtype->obj.oid.lo)
        UINT64DECODE(p, dtype->obj.oid.hi)

        /* Decode serialized info lengths */
        UINT64DECODE(p, type_len)
        UINT64DECODE(p, tcpl_len)
        tot_len = type_len + tcpl_len;

        /* Check for type_len set to 0 - indicates failure */
        if(type_len == 0)
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTINIT, NULL, "lead process failed to open datatype")

        /* Check if we need to perform another bcast */
        if(tot_len + (4 * sizeof(uint64_t)) > sizeof(tinfo_buf_static)) {
            /* Allocate a dynamic buffer if necessary */
            if(tot_len > sizeof(tinfo_buf_static)) {
                if(NULL == (tinfo_buf_dyn = (uint8_t *)DV_malloc(tot_len)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate space for datatype info")
                tinfo_buf = tinfo_buf_dyn;
            } /* end if */

            /* Receive datatype info */
            if(MPI_SUCCESS != MPI_Bcast((char *)tinfo_buf, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_DATATYPE, H5E_MPI, NULL, "can't bcast datatype info (second bcast)")

            p = tinfo_buf;
        } /* end if */

        /* Open datatype */
        if(0 != (ret = daos_obj_open(item->file->coh, dtype->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &dtype->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTOPENOBJ, NULL, "can't open datatype: %d", ret)
    } /* end else */

    /* Decode datatype and TCPL */
    if((dtype->type_id = H5Tdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")
    p += type_len;
    if((dtype->tcpl_id = H5Pdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype creation property list")

    /* Finish setting up datatype struct */
    if((dtype->tapl_id = H5Pcopy(tapl_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy tapl");

    /* Set return value */
    ret_value = (void *)dtype;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast tinfo_buf as '0' if necessary - this will trigger failures in
         * in other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(tinfo_buf_static, 0, sizeof(tinfo_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(tinfo_buf_static, sizeof(tinfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_DONE_ERROR(H5E_DATATYPE, H5E_MPI, NULL, "can't bcast empty datatype info")
        } /* end if */

        /* Close datatype */
        if(dtype && H5_daos_datatype_close(dtype, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, NULL, "can't close datatype")
    } /* end if */

    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, NULL, "can't close group")

    /* Free memory */
    tinfo_buf_dyn = (uint8_t *)DV_free(tinfo_buf_dyn);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_datatype_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_datatype_get
 *
 * Purpose:     Gets certain information about a datatype
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_datatype_get(void *_dtype, H5VL_datatype_get_t get_type,
    hid_t DV_ATTR_UNUSED dxpl_id, void DV_ATTR_UNUSED **req, va_list arguments)
{
    H5_daos_dtype_t *dtype = (H5_daos_dtype_t *)_dtype;
    herr_t       ret_value = SUCCEED;    /* Return value */

    switch (get_type) {
        case H5VL_DATATYPE_GET_BINARY:
            {
                ssize_t *nalloc = va_arg(arguments, ssize_t *);
                void *buf = va_arg(arguments, void *);
                size_t size = va_arg(arguments, size_t);

                if(H5Tencode(dtype->type_id, buf, &size) < 0)
                    D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "can't determine serialized length of datatype")

                *nalloc = (ssize_t)size;
                break;
            } /* end block */
        case H5VL_DATATYPE_GET_TCPL:
            {
                hid_t *plist_id = va_arg(arguments, hid_t *);

                /* Retrieve the datatype's creation property list */
                if((*plist_id = H5Pcopy(dtype->tcpl_id)) < 0)
                    D_GOTO_ERROR(H5E_DATATYPE, H5E_CANTGET, FAIL, "can't get dtype creation property list")

                break;
            } /* end block */
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "can't get this type of information from datatype")
    } /* end switch */

done:
    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_datatype_get() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_datatype_close
 *
 * Purpose:     Closes a DAOS HDF5 datatype.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_datatype_close(void *_dtype, hid_t DV_ATTR_UNUSED dxpl_id,
    void DV_ATTR_UNUSED **req)
{
    H5_daos_dtype_t *dtype = (H5_daos_dtype_t *)_dtype;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(dtype);

    if(--dtype->obj.item.rc == 0) {
        /* Free datatype data structures */
        if(!daos_handle_is_inval(dtype->obj.obj_oh))
            if(0 != (ret = daos_obj_close(dtype->obj.obj_oh, NULL /*event*/)))
                D_DONE_ERROR(H5E_DATATYPE, H5E_CANTCLOSEOBJ, FAIL, "can't close datatype DAOS object: %d", ret)
        if(dtype->type_id != FAIL && H5Idec_ref(dtype->type_id) < 0)
            D_DONE_ERROR(H5E_DATATYPE, H5E_CANTDEC, FAIL, "failed to close datatype")
        if(dtype->tcpl_id != FAIL && H5Idec_ref(dtype->tcpl_id) < 0)
            D_DONE_ERROR(H5E_DATATYPE, H5E_CANTDEC, FAIL, "failed to close plist")
        if(dtype->tapl_id != FAIL && H5Idec_ref(dtype->tapl_id) < 0)
            D_DONE_ERROR(H5E_DATATYPE, H5E_CANTDEC, FAIL, "failed to close plist")
        dtype = H5FL_FREE(H5_daos_dtype_t, dtype);
    } /* end if */

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_datatype_close() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_object_open
 *
 * Purpose:     Opens a DAOS HDF5 object.
 *
 * Return:      Success:        object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_object_open(void *_item, const H5VL_loc_params_t *loc_params,
    H5I_type_t *opened_type, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_obj_t *obj = NULL;
    H5_daos_group_t *target_grp = NULL;
    const char *target_name = NULL;
    daos_obj_id_t oid;
    uint8_t oid_buf[2 * sizeof(uint64_t)];
    uint8_t *p;
    hbool_t collective = item->file->collective;
    hbool_t must_bcast = FALSE;
    H5I_type_t obj_type;
    H5VL_loc_params_t sub_loc_params;
    void *ret_value = NULL;

    /*
     * DSINC - should probably use a major error code other than
     * object headers for H5O calls.
     */
    if(H5VL_OBJECT_BY_IDX == loc_params->type)
        D_GOTO_ERROR(H5E_OHDR, H5E_UNSUPPORTED, NULL, "H5Oopen_by_idx is unsupported")

    /* Check loc_params type */
    if(H5VL_OBJECT_BY_ADDR == loc_params->type) {
        /* Get object type */
        if(H5I_BADID == (obj_type = H5_daos_addr_to_type((uint64_t)loc_params->loc_data.loc_by_addr.addr)))
            D_GOTO_ERROR(H5E_OHDR, H5E_CANTINIT, NULL, "can't get object type")

        /* Generate oid from address */
        memset(&oid, 0, sizeof(oid));
        H5_daos_oid_generate(&oid, (uint64_t)loc_params->loc_data.loc_by_addr.addr, obj_type);
    } /* end if */
    else {
        assert(H5VL_OBJECT_BY_NAME == loc_params->type);

        /* Check for collective access, if not already set by the file */
        if(!collective)
            if(H5Pget_all_coll_metadata_ops(loc_params->loc_data.loc_by_name.lapl_id, &collective) < 0)
                D_GOTO_ERROR(H5E_OHDR, H5E_CANTGET, NULL, "can't get collective access property")

        /* Check if we're actually opening the group or just receiving the group
         * info from the leader */
        if(!collective || (item->file->my_rank == 0)) {
            if(collective && (item->file->num_procs > 1))
                must_bcast = TRUE;

            /* Traverse the path */
            if(NULL == (target_grp = H5_daos_group_traverse(item, loc_params->loc_data.loc_by_name.name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_OHDR, H5E_BADITER, NULL, "can't traverse path")

            /* Check for no target_name, in this case just reopen target_grp */
            if(target_name[0] == '\0'
                    || (target_name[0] == '.' && target_name[1] == '\0'))
                oid = target_grp->obj.oid;
            else
                /* Follow link to object */
                if(H5_daos_link_follow(target_grp, target_name, strlen(target_name), dxpl_id, req, &oid) < 0)
                    D_GOTO_ERROR(H5E_OHDR, H5E_CANTINIT, NULL, "can't follow link to group")

            /* Broadcast group info if there are other processes that need it */
            if(collective && (item->file->num_procs > 1)) {
                /* Encode oid */
                p = oid_buf;
                UINT64ENCODE(p, oid.lo)
                UINT64ENCODE(p, oid.hi)

                /* We are about to bcast so we no longer need to bcast on failure */
                must_bcast = FALSE;

                /* MPI_Bcast oid_buf */
                if(MPI_SUCCESS != MPI_Bcast((char *)oid_buf, sizeof(oid_buf), MPI_BYTE, 0, item->file->comm))
                    D_GOTO_ERROR(H5E_OHDR, H5E_MPI, NULL, "can't bcast object id")
            } /* end if */
        } /* end if */
        else {
            /* Receive oid_buf */
            if(MPI_SUCCESS != MPI_Bcast((char *)oid_buf, sizeof(oid_buf), MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_OHDR, H5E_MPI, NULL, "can't bcast object id")

            /* Decode oid */
            p = oid_buf;
            UINT64DECODE(p, oid.lo)
            UINT64DECODE(p, oid.hi)

            /* Check for oid.lo set to 0 - indicates failure */
            if(oid.lo == 0)
                D_GOTO_ERROR(H5E_OHDR, H5E_CANTINIT, NULL, "lead process failed to open object")
        } /* end else */

        /* Get object type */
        if(H5I_BADID == (obj_type = H5_daos_oid_to_type(oid)))
            D_GOTO_ERROR(H5E_OHDR, H5E_CANTINIT, NULL, "can't get object type")
    } /* end else */

    /* Set up sub_loc_params */
    sub_loc_params.obj_type = item->type;
    sub_loc_params.type = H5VL_OBJECT_BY_ADDR;
    sub_loc_params.loc_data.loc_by_addr.addr = (haddr_t)oid.lo;

    /* Call type's open function */
    if(obj_type == H5I_GROUP) {
        if(NULL == (obj = (H5_daos_obj_t *)H5_daos_group_open(item, &sub_loc_params, NULL,
                ((H5VL_OBJECT_BY_NAME == loc_params->type) && (loc_params->loc_data.loc_by_name.lapl_id != H5P_DEFAULT))
                ? loc_params->loc_data.loc_by_name.lapl_id : H5P_GROUP_ACCESS_DEFAULT, dxpl_id, req)))
            D_GOTO_ERROR(H5E_OHDR, H5E_CANTOPENOBJ, NULL, "can't open group")
    } /* end if */
    else if(obj_type == H5I_DATASET) {
        if(NULL == (obj = (H5_daos_obj_t *)H5_daos_dataset_open(item, &sub_loc_params, NULL,
                ((H5VL_OBJECT_BY_NAME == loc_params->type) && (loc_params->loc_data.loc_by_name.lapl_id != H5P_DEFAULT))
                ? loc_params->loc_data.loc_by_name.lapl_id : H5P_DATASET_ACCESS_DEFAULT, dxpl_id, req)))
            D_GOTO_ERROR(H5E_OHDR, H5E_CANTOPENOBJ, NULL, "can't open dataset")
    } /* end if */
    else if(obj_type == H5I_DATATYPE) {
        if(NULL == (obj = (H5_daos_obj_t *)H5_daos_datatype_open(item, &sub_loc_params, NULL,
                ((H5VL_OBJECT_BY_NAME == loc_params->type) && (loc_params->loc_data.loc_by_name.lapl_id != H5P_DEFAULT))
                ? loc_params->loc_data.loc_by_name.lapl_id : H5P_DATATYPE_ACCESS_DEFAULT, dxpl_id, req)))
            D_GOTO_ERROR(H5E_OHDR, H5E_CANTOPENOBJ, NULL, "can't open datatype")
    } /* end if */
    else {
#ifdef DV_HAVE_MAP
        assert(obj_type == H5I_MAP);
        if(NULL == (obj = (H5_daos_obj_t *)H5_daos_map_open(item, sub_loc_params, NULL,
                ((H5VL_OBJECT_BY_NAME == loc_params.type) && (loc_params.loc_data.loc_by_name.lapl_id != H5P_DEFAULT))
                ? loc_params.loc_data.loc_by_name.lapl_id : H5P_MAP_ACCESS_DEFAULT, dxpl_id, req)))
            D_GOTO_ERROR(H5E_OHDR, H5E_CANTOPENOBJ, NULL, "can't open map")
#endif
    } /* end if */

    /* Set return value */
    if(opened_type)
        *opened_type = obj_type;
    ret_value = (void *)obj;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast oid_buf as '0' if necessary - this will trigger failures in
         * other processes */
        if(must_bcast) {
            memset(oid_buf, 0, sizeof(oid_buf));
            if(MPI_SUCCESS != MPI_Bcast(oid_buf, sizeof(oid_buf), MPI_BYTE, 0, item->file->comm))
                D_DONE_ERROR(H5E_OHDR, H5E_MPI, NULL, "can't bcast empty object id")
        } /* end if */

        /* Close object */
        if(obj && H5_daos_object_close(obj, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_OHDR, H5E_CLOSEERROR, NULL, "can't close object")
    } /* end if */

    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_OHDR, H5E_CLOSEERROR, NULL, "can't close group")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_object_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_object_optional
 *
 * Purpose:     Optional operations with objects
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_object_optional(void *_item, hid_t dxpl_id, void **req,
    va_list arguments)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_obj_t *target_obj = NULL;
    H5VL_object_optional_t optional_type = (H5VL_object_optional_t)va_arg(arguments, int);
    H5VL_loc_params_t *loc_params = va_arg(arguments, H5VL_loc_params_t *);
    char *akey_buf = NULL;
    size_t akey_buf_len = 0;
    int ret;
    herr_t ret_value = SUCCEED;    /* Return value */

    /* Determine target object */
    if(loc_params->type == H5VL_OBJECT_BY_SELF) {
        /* Use item as attribute parent object, or the root group if item is a
         * file */
        if(item->type == H5I_FILE)
            target_obj = (H5_daos_obj_t *)((H5_daos_file_t *)item)->root_grp;
        else
            target_obj = (H5_daos_obj_t *)item;
        target_obj->item.rc++;
    } /* end if */
    else if(loc_params->type == H5VL_OBJECT_BY_NAME) {
        /* Open target_obj */
        if(NULL == (target_obj = (H5_daos_obj_t *)H5_daos_object_open(item, loc_params, NULL, dxpl_id, req)))
            D_GOTO_ERROR(H5E_OHDR, H5E_CANTOPENOBJ, FAIL, "can't open object")
    } /* end else */
    else
        D_GOTO_ERROR(H5E_OHDR, H5E_UNSUPPORTED, FAIL, "unsupported object operation location parameters type")

    switch (optional_type) {
        /* H5Oget_info / H5Oget_info_by_name / H5Oget_info_by_idx */
        case H5VL_OBJECT_GET_INFO:
            {
                H5O_info_t  *obj_info = va_arg(arguments, H5O_info_t *);
                unsigned fields = va_arg(arguments, unsigned);
                uint64_t fileno64;
                uint8_t *uuid_p = (uint8_t *)&target_obj->item.file->uuid;

                /* Initialize obj_info - most fields are not valid and will
                 * simply be set to 0 */
                memset(obj_info, 0, sizeof(*obj_info));

                /* Fill in valid fields of obj_info */
                /* Basic fields */
                if(fields & H5O_INFO_BASIC) {
                    /* Use the lower <sizeof(unsigned long)> bytes of the file uuid
                     * as the fileno.  Ideally we would write separate 32 and 64 bit
                     * hash functions but this should work almost as well. */
                    UINT64DECODE(uuid_p, fileno64)
                    obj_info->fileno = (unsigned long)fileno64;

                    /* Use lower 64 bits of oid as address - contains encode object
                     * type */
                    obj_info->addr = (haddr_t)target_obj->oid.lo;

                    /* Set object type */
                    if(target_obj->item.type == H5I_GROUP)
                        obj_info->type = H5O_TYPE_GROUP;
                    else if(target_obj->item.type == H5I_DATASET)
                        obj_info->type = H5O_TYPE_DATASET;
                    else if(target_obj->item.type == H5I_DATATYPE)
                        obj_info->type = H5O_TYPE_NAMED_DATATYPE;
                    else {
#ifdef DV_HAVE_MAP
                        assert(target_obj->item.type == H5I_MAP);
                        obj_info->type = H5O_TYPE_MAP;
#else
                        obj_info->type = H5O_TYPE_UNKNOWN;
#endif
                    } /* end else */

                    /* Reference count is always 1 - change this when
                     * H5Lcreate_hard() is implemented */
                    obj_info->rc = 1;
                } /* end if */

                /* Number of attributes. */
                if(fields & H5O_INFO_NUM_ATTRS) {
                    daos_anchor_t anchor;
                    uint32_t nr;
                    daos_key_t dkey;
                    daos_key_desc_t kds[H5_DAOS_ITER_LEN];
                    daos_sg_list_t sgl;
                    daos_iov_t sg_iov;
                    char attr_key[] = H5_DAOS_ATTR_KEY;
                    char *p;
                    uint32_t i;

                    /* Initialize anchor */
                    memset(&anchor, 0, sizeof(anchor));

                    /* Set up dkey */
                    daos_iov_set(&dkey, attr_key, (daos_size_t)(sizeof(attr_key) - 1));

                    /* Allocate akey_buf */
                    if(NULL == (akey_buf = (char *)DV_malloc(H5_DAOS_ITER_SIZE_INIT)))
                        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for dkeys")
                    akey_buf_len = H5_DAOS_ITER_SIZE_INIT;

                    /* Set up sgl */
                    daos_iov_set(&sg_iov, akey_buf, (daos_size_t)akey_buf_len );
                    sgl.sg_nr = 1;
                    sgl.sg_nr_out = 0;
                    sgl.sg_iovs = &sg_iov;

                    /* Loop to retrieve keys and make callbacks */
                    do {
                        /* Loop to retrieve keys (exit as soon as we get at least 1
                         * key) */
                        do {
                            /* Reset nr */
                            nr = H5_DAOS_ITER_LEN;

                            /* Ask daos for a list of akeys, break out if we succeed
                             */
                            if(0 == (ret = daos_obj_list_akey(target_obj->obj_oh, DAOS_TX_NONE, &dkey, &nr, kds, &sgl, &anchor, NULL /*event*/)))
                                break;

                            /* Call failed, if the buffer is too small double it and
                             * try again, otherwise fail */
                            if(ret == -DER_KEY2BIG) {
                                /* Allocate larger buffer */
                                DV_free(akey_buf);
                                akey_buf_len *= 2;
                                if(NULL == (akey_buf = (char *)DV_malloc(akey_buf_len)))
                                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akeys")

                                /* Update sgl */
                                daos_iov_set(&sg_iov, akey_buf, (daos_size_t)akey_buf_len);
                            } /* end if */
                            else
                                D_GOTO_ERROR(H5E_SYM, H5E_CANTGET, FAIL, "can't list attributes: %d", ret)
                        } while(1);

                        /* Count number of returned attributes */
                        p = akey_buf;
                        for(i = 0; i < nr; i++) {
                            /* Check for invalid key */
                            if(kds[i].kd_key_len < 3)
                                D_GOTO_ERROR(H5E_ATTR, H5E_CANTDECODE, FAIL, "attribute akey too short")
                            if(p[1] != '-')
                                D_GOTO_ERROR(H5E_ATTR, H5E_CANTDECODE, FAIL, "invalid attribute akey format")

                            /* Only count for "S-" (dataspace) keys, to avoid
                             * duplication */
                            if(p[0] == 'S')
                                obj_info->num_attrs ++;

                            /* Advance to next akey */
                            p += kds[i].kd_key_len + kds[i].kd_csum_len;
                        } /* end for */
                    } while(!daos_anchor_is_eof(&anchor));
                } /* end if */
                /* Investigate collisions with links, etc DAOSINC */
                break;
            } /* end block */

        case H5VL_OBJECT_GET_COMMENT:
        case H5VL_OBJECT_SET_COMMENT:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "unsupported optional operation")
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_BADVALUE, FAIL, "invalid optional operation")
    } /* end switch */

done:
    if(target_obj) {
        if(H5_daos_object_close(target_obj, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_OHDR, H5E_CLOSEERROR, FAIL, "can't close object")
        target_obj = NULL;
    } /* end else */
    akey_buf = (char *)DV_free(akey_buf);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_object_optional() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_object_close
 *
 * Purpose:     Closes a DAOS HDF5 object.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_object_close(void *_obj, hid_t dxpl_id, void **req)
{
    H5_daos_obj_t *obj = (H5_daos_obj_t *)_obj;
    herr_t ret_value = SUCCEED;

    assert(obj);
#ifdef DV_HAVE_MAP
    /* DSINC - cannot place #ifdef inside assert macro - compiler warning
     * 'embedding a directive within macro arguments is not portable'
     */
    assert(obj->item.type == H5I_GROUP || obj->item.type == H5I_DATASET
            || obj->item.type == H5I_DATATYPE || obj->item.type == H5I_MAP
    );
#else
    assert(obj->item.type == H5I_GROUP || obj->item.type == H5I_DATASET
                || obj->item.type == H5I_DATATYPE);
#endif

    /* Call type's close function */
    if(obj->item.type == H5I_GROUP) {
        if(H5_daos_group_close(obj, dxpl_id, req))
            D_GOTO_ERROR(H5E_SYM, H5E_CLOSEERROR, FAIL, "can't close group")
    } /* end if */
    else if(obj->item.type == H5I_DATASET) {
        if(H5_daos_dataset_close(obj, dxpl_id, req))
            D_GOTO_ERROR(H5E_DATASET, H5E_CLOSEERROR, FAIL, "can't close dataset")
    } /* end if */
    else if(obj->item.type == H5I_DATATYPE) {
        if(H5_daos_datatype_close(obj, dxpl_id, req))
            D_GOTO_ERROR(H5E_DATATYPE, H5E_CLOSEERROR, FAIL, "can't close datatype")
    } /* end if */
#ifdef DV_HAVE_MAP
    else if(obj->item.type == H5I_MAP) {
        if(H5_daos_map_close(obj, dxpl_id, req))
            D_GOTO_ERROR(H5E_MAP, H5E_CLOSEERROR, FAIL, "can't close map")
    } /* end if */
#endif
    else
        assert(0 && "Invalid object type");

done:
    D_FUNC_LEAVE
} /* end H5_daos_object_close() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_attribute_create
 *
 * Purpose:     Sends a request to DAOS to create an attribute
 *
 * Return:      Success:        attribute object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_attribute_create(void *_item, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t acpl_id, hid_t DV_ATTR_UNUSED aapl_id,
    hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_attr_t *attr = NULL;
    size_t akey_len;
    hid_t type_id, space_id;
    daos_key_t dkey;
    char *type_key = NULL;
    char *space_key = NULL;
    daos_iod_t iod[2];
    daos_sg_list_t sgl[2];
    daos_iov_t sg_iov[2];
    size_t type_size = 0;
    size_t space_size = 0;
    void *type_buf = NULL;
    void *space_buf = NULL;
    char attr_key[] = H5_DAOS_ATTR_KEY;
    int ret;
    void *ret_value = NULL;

    /* Check for write access */
    if(!(item->file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, NULL, "no write intent on file")

    /* get creation properties */
    if(H5Pget(acpl_id, H5VL_PROP_ATTR_TYPE_ID, &type_id) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, NULL, "can't get property value for datatype id")
    if(H5Pget(acpl_id, H5VL_PROP_ATTR_SPACE_ID, &space_id) < 0)
        D_GOTO_ERROR(H5E_PLIST, H5E_CANTGET, NULL, "can't get property value for space id")

    /* Allocate the attribute object that is returned to the user */
    if(NULL == (attr = H5FL_CALLOC(H5_daos_attr_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS dataset struct")
    attr->item.type = H5I_ATTR;
    attr->item.file = item->file;
    attr->item.rc = 1;
    attr->type_id = FAIL;
    attr->space_id = FAIL;

    /* Determine attribute object */
    if(loc_params->type == H5VL_OBJECT_BY_SELF) {
        /* Use item as attribute parent object, or the root group if item is a
         * file */
        if(item->type == H5I_FILE)
            attr->parent = (H5_daos_obj_t *)((H5_daos_file_t *)item)->root_grp;
        else
            attr->parent = (H5_daos_obj_t *)item;
        attr->parent->item.rc++;
    } /* end if */
    else if(loc_params->type == H5VL_OBJECT_BY_NAME) {
        /* Open target_obj */
        if(NULL == (attr->parent = (H5_daos_obj_t *)H5_daos_object_open(item, loc_params, NULL, dxpl_id, req)))
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTOPENOBJ, NULL, "can't open object for attribute")
    } /* end else */
    else
        D_GOTO_ERROR(H5E_ATTR, H5E_UNSUPPORTED, NULL, "unsupported attribute create location parameters type")

    /* Encode datatype */
    if(H5Tencode(type_id, NULL, &type_size) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of datatype")
    if(NULL == (type_buf = DV_malloc(type_size)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized datatype")
    if(H5Tencode(type_id, type_buf, &type_size) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize datatype")

    /* Encode dataspace */
    if(H5Sencode(space_id, NULL, &space_size) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of dataspace")
    if(NULL == (space_buf = DV_malloc(space_size)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized dataspace")
    if(H5Sencode(space_id, space_buf, &space_size) < 0)
        D_GOTO_ERROR(H5E_DATASET, H5E_CANTENCODE, NULL, "can't serialize dataspace")

    /* Set up operation to write datatype and dataspace to attribute */
    /* Set up dkey */
    daos_iov_set(&dkey, attr_key, (daos_size_t)(sizeof(attr_key) - 1));

    /* Create akey strings (prefix "S-", "T-") */
    akey_len = strlen(name) + 2;
    if(NULL == (type_key = (char *)DV_malloc(akey_len + 1)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for akey")
    if(NULL == (space_key = (char *)DV_malloc(akey_len + 1)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for akey")
    type_key[0] = 'T';
    type_key[1] = '-';
    space_key[0] = 'S';
    space_key[1] = '-';
    (void)strcpy(type_key + 2, name);
    (void)strcpy(space_key + 2, name);

    /* Set up iod */
    memset(iod, 0, sizeof(iod));
    daos_iov_set(&iod[0].iod_name, (void *)type_key, (daos_size_t)akey_len);
    daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
    iod[0].iod_nr = 1u;
    iod[0].iod_size = (uint64_t)type_size;
    iod[0].iod_type = DAOS_IOD_SINGLE;

    daos_iov_set(&iod[1].iod_name, (void *)space_key, (daos_size_t)akey_len);
    daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
    iod[1].iod_nr = 1u;
    iod[1].iod_size = (uint64_t)space_size;
    iod[1].iod_type = DAOS_IOD_SINGLE;

    /* Set up sgl */
    daos_iov_set(&sg_iov[0], type_buf, (daos_size_t)type_size);
    sgl[0].sg_nr = 1;
    sgl[0].sg_nr_out = 0;
    sgl[0].sg_iovs = &sg_iov[0];
    daos_iov_set(&sg_iov[1], space_buf, (daos_size_t)space_size);
    sgl[1].sg_nr = 1;
    sgl[1].sg_nr_out = 0;
    sgl[1].sg_iovs = &sg_iov[1];

    /* Write attribute metadata to parent object */
    if(0 != (ret = daos_obj_update(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, 2, iod, sgl, NULL /*event*/)))
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTINIT, NULL, "can't write attribute metadata: %d", ret)

    /* Finish setting up attribute struct */
    if(NULL == (attr->name = strdup(name)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't copy attribute name")
    if((attr->type_id = H5Tcopy(type_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy datatype")
    if((attr->space_id = H5Scopy(space_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy dataspace")
    if(H5Sselect_all(attr->space_id) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTDELETE, NULL, "can't change selection")

    ret_value = (void *)attr;

done:
    /* Free memory */
    type_buf = DV_free(type_buf);
    space_buf = DV_free(space_buf);
    type_key = (char *)DV_free(type_key);
    space_key = (char *)DV_free(space_key);

    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close attribute */
        if(attr && H5_daos_attribute_close(attr, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, NULL, "can't close attribute")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_attribute_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_attribute_open
 *
 * Purpose:     Sends a request to DAOS to open an attribute
 *
 * Return:      Success:        attribute object. 
 *              Failure:        NULL
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static void *
H5_daos_attribute_open(void *_item, const H5VL_loc_params_t *loc_params,
    const char *name, hid_t DV_ATTR_UNUSED aapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_attr_t *attr = NULL;
    size_t akey_len;
    daos_key_t dkey;
    char *type_key = NULL;
    char *space_key = NULL;
    daos_iod_t iod[2];
    daos_sg_list_t sgl[2];
    daos_iov_t sg_iov[2];
    void *type_buf = NULL;
    void *space_buf = NULL;
    char attr_key[] = H5_DAOS_ATTR_KEY;
    int ret;
    void *ret_value = NULL;

    /* Allocate the attribute object that is returned to the user */
    if(NULL == (attr = H5FL_CALLOC(H5_daos_attr_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS dataset struct")
    attr->item.type = H5I_ATTR;
    attr->item.file = item->file;
    attr->item.rc = 1;
    attr->type_id = FAIL;
    attr->space_id = FAIL;

    /* Determine attribute object */
    if(loc_params->type == H5VL_OBJECT_BY_SELF) {
        /* Use item as attribute parent object, or the root group if item is a
         * file */
        if(item->type == H5I_FILE)
            attr->parent = (H5_daos_obj_t *)((H5_daos_file_t *)item)->root_grp;
        else
            attr->parent = (H5_daos_obj_t *)item;
        attr->parent->item.rc++;
    } /* end if */
    else if(loc_params->type == H5VL_OBJECT_BY_NAME) {
        /* Open target_obj */
        if(NULL == (attr->parent = (H5_daos_obj_t *)H5_daos_object_open(item, loc_params, NULL, dxpl_id, req)))
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTOPENOBJ, NULL, "can't open object for attribute")
    } /* end else */
    else
        D_GOTO_ERROR(H5E_ATTR, H5E_UNSUPPORTED, NULL, "unsupported attribute open location parameters type")

    /* Set up operation to write datatype and dataspace to attribute */
    /* Set up dkey */
    daos_iov_set(&dkey, attr_key, (daos_size_t)(sizeof(attr_key) - 1));

    /* Create akey strings (prefix "S-", "T-") */
    akey_len = strlen(name) + 2;
    if(NULL == (type_key = (char *)DV_malloc(akey_len + 1)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for akey")
    if(NULL == (space_key = (char *)DV_malloc(akey_len + 1)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for akey")
    type_key[0] = 'T';
    type_key[1] = '-';
    space_key[0] = 'S';
    space_key[1] = '-';
    (void)strcpy(type_key + 2, name);
    (void)strcpy(space_key + 2, name);

    /* Set up iod */
    memset(iod, 0, sizeof(iod));
    daos_iov_set(&iod[0].iod_name, (void *)type_key, (daos_size_t)akey_len);
    daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
    iod[0].iod_nr = 1u;
    iod[0].iod_size = DAOS_REC_ANY;
    iod[0].iod_type = DAOS_IOD_SINGLE;

    daos_iov_set(&iod[1].iod_name, (void *)space_key, (daos_size_t)akey_len);
    daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
    iod[1].iod_nr = 1u;
    iod[1].iod_size = DAOS_REC_ANY;
    iod[1].iod_type = DAOS_IOD_SINGLE;

    /* Read attribute metadata sizes from parent object */
    if(0 != (ret = daos_obj_fetch(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, 2, iod, NULL, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTDECODE, NULL, "can't read attribute metadata sizes: %d", ret)

    if(iod[0].iod_size == (uint64_t)0 || iod[1].iod_size == (uint64_t)0)
        D_GOTO_ERROR(H5E_ATTR, H5E_NOTFOUND, NULL, "attribute not found")

    /* Allocate buffers for datatype and dataspace */
    if(NULL == (type_buf = DV_malloc(iod[0].iod_size)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized datatype")
    if(NULL == (space_buf = DV_malloc(iod[1].iod_size)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized dataaspace")

    /* Set up sgl */
    daos_iov_set(&sg_iov[0], type_buf, (daos_size_t)iod[0].iod_size);
    sgl[0].sg_nr = 1;
    sgl[0].sg_nr_out = 0;
    sgl[0].sg_iovs = &sg_iov[0];
    daos_iov_set(&sg_iov[1], space_buf, (daos_size_t)iod[1].iod_size);
    sgl[1].sg_nr = 1;
    sgl[1].sg_nr_out = 0;
    sgl[1].sg_iovs = &sg_iov[1];

    /* Read attribute metadata from parent object */
    if(0 != (ret = daos_obj_fetch(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, 2, iod, sgl, NULL /*maps*/, NULL /*event*/)))
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTDECODE, NULL, "can't read attribute metadata: %d", ret)

    /* Decode datatype and dataspace */
    if((attr->type_id = H5Tdecode(type_buf)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")
    if((attr->space_id = H5Sdecode(space_buf)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")
    if(H5Sselect_all(attr->space_id) < 0)
        D_GOTO_ERROR(H5E_DATASPACE, H5E_CANTDELETE, NULL, "can't change selection")

    /* Finish setting up attribute struct */
    if(NULL == (attr->name = strdup(name)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't copy attribute name")

    ret_value = (void *)attr;

done:
    /* Free memory */
    type_buf = DV_free(type_buf);
    space_buf = DV_free(space_buf);
    type_key = (char *)DV_free(type_key);
    space_key = (char *)DV_free(space_key);

    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close attribute */
        if(attr && H5_daos_attribute_close(attr, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, NULL, "can't close attribute")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_attribute_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_attribute_read
 *
 * Purpose:     Reads raw data from an attribute into a buffer.
 *
 * Return:      Success:        0
 *              Failure:        -1, attribute not read.
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_attribute_read(void *_attr, hid_t mem_type_id, void *buf,
    hid_t dxpl_id, void DV_ATTR_UNUSED **req)
{
    H5_daos_attr_t *attr = (H5_daos_attr_t *)_attr;
    int ndims;
    hsize_t dim[H5S_MAX_RANK];
    size_t akey_len;
    daos_key_t dkey;
    char *akey = NULL;
    uint8_t **akeys = NULL;
    daos_iod_t *iods = NULL;
    daos_sg_list_t *sgls = NULL;
    daos_iov_t *sg_iovs = NULL;
    char attr_key[] = H5_DAOS_ATTR_KEY;
    hid_t base_type_id = FAIL;
    size_t base_type_size = 0;
    uint64_t attr_size;
    void *tconv_buf = NULL;
    void *bkg_buf = NULL;
    H5T_class_t type_class;
    hbool_t is_vl = FALSE;
    htri_t is_vl_str = FALSE;
    int ret;
    uint64_t i;
    herr_t ret_value = SUCCEED;

    if(!buf)
        D_GOTO_ERROR(H5E_ATTR, H5E_BADVALUE, FAIL, "read buffer is NULL")

    /* Get dataspace extent */
    if((ndims = H5Sget_simple_extent_ndims(attr->space_id)) < 0)
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get number of dimensions")
    if(ndims != H5Sget_simple_extent_dims(attr->space_id, dim, NULL))
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get dimensions")

    /* Calculate attribute size */
    attr_size = (uint64_t)1;
    for(i = 0; i < (uint64_t)ndims; i++)
        attr_size *= (uint64_t)dim[i];

    /* Set up dkey */
    daos_iov_set(&dkey, attr_key, (daos_size_t)(sizeof(attr_key) - 1));

    /* Check for vlen */
    if(H5T_NO_CLASS == (type_class = H5Tget_class(mem_type_id)))
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get datatype class")
    if(type_class == H5T_VLEN) {
        is_vl = TRUE;

        /* Calculate base type size */
        if((base_type_id = H5Tget_super(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get datatype base type")
        if(0 == (base_type_size = H5Tget_size(base_type_id)))
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get datatype base type size")
    } /* end if */
    else if(type_class == H5T_STRING) {
        /* check for vlen string */
        if((is_vl_str = H5Tis_variable_str(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't check for variable length string")
        if(is_vl_str)
            is_vl = TRUE;
    } /* end if */

    /* Check for variable length */
    if(is_vl) {
        size_t akey_str_len;
        uint64_t offset = 0;
        uint8_t *p;

        /* Calculate akey length */
        akey_str_len = strlen(attr->name) + 2;
        akey_len = akey_str_len + sizeof(uint64_t);

        /* Allocate array of akey pointers */
        if(NULL == (akeys = (uint8_t **)DV_calloc(attr_size * sizeof(uint8_t *))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey array")

        /* Allocate array of iods */
        if(NULL == (iods = (daos_iod_t *)DV_calloc(attr_size * sizeof(daos_iod_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for I/O descriptor array")

        /* First loop over elements, set up operation to read vl sizes */
        for(i = 0; i < attr_size; i++) {
            /* Create akey for this element */
            if(NULL == (akeys[i] = (uint8_t *)DV_malloc(akey_len)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey")
            akeys[i][0] = 'V';
            akeys[i][1] = '-';
            (void)strcpy((char *)akeys[i] + 2, attr->name);
            p = akeys[i] + akey_str_len;
            UINT64ENCODE(p, i)

            /* Set up iod.  Use "single" records of varying size. */
            daos_iov_set(&iods[i].iod_name, (void *)akeys[i], (daos_size_t)akey_len);
            daos_csum_set(&iods[i].iod_kcsum, NULL, 0);
            iods[i].iod_nr = 1u;
            iods[i].iod_size = DAOS_REC_ANY;
            iods[i].iod_type = DAOS_IOD_SINGLE;
        } /* end for */

        /* Read vl sizes from attribute */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_fetch(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, (unsigned)attr_size, iods, NULL, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_ATTR, H5E_READERROR, FAIL, "can't read vl data sizes from attribute: %d", ret)

        /* Allocate array of sg_iovs */
        if(NULL == (sg_iovs = (daos_iov_t *)DV_malloc(attr_size * sizeof(daos_iov_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list")

        /* Allocate array of sgls */
        if(NULL == (sgls = (daos_sg_list_t *)DV_malloc(attr_size * sizeof(daos_sg_list_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list array")

        /* Second loop over elements, set up operation to read vl data */
        for(i = 0; i < attr_size; i++) {
            /* Set up constant sgl info */
            sgls[i].sg_nr = 1;
            sgls[i].sg_nr_out = 0;
            sgls[i].sg_iovs = &sg_iovs[i];

            /* Check for empty element */
            if(iods[i].iod_size == 0) {
                /* Increment offset, slide down following elements */
                offset++;

                /* Zero out read buffer */
                if(is_vl_str)
                    ((char **)buf)[i] = NULL;
                else
                    memset(&((hvl_t *)buf)[i], 0, sizeof(hvl_t));
            } /* end if */
            else {
                assert(i >= offset);

                /* Check for vlen string */
                if(is_vl_str) {
                    char *elem = NULL;

                    /* Allocate buffer for this vl element */
                    if(NULL == (elem = (char *)malloc((size_t)iods[i].iod_size + 1)))
                        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate vl data buffer")
                    ((char **)buf)[i] = elem;

                    /* Add null terminator */
                    elem[iods[i].iod_size] = '\0';

                    /* Set buffer location in sgl */
                    daos_iov_set(&sg_iovs[i - offset], elem, iods[i].iod_size);
                } /* end if */
                else {
                    /* Standard vlen, find hvl_t struct for this element */
                    hvl_t *elem = &((hvl_t *)buf)[i];

                    assert(base_type_size > 0);

                    /* Allocate buffer for this vl element and set size */
                    elem->len = (size_t)iods[i].iod_size / base_type_size;
                    if(NULL == (elem->p = malloc((size_t)iods[i].iod_size)))
                        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate vl data buffer")

                    /* Set buffer location in sgl */
                    daos_iov_set(&sg_iovs[i - offset], elem->p, iods[i].iod_size);
                } /* end if */

                /* Slide down iod if necessary */
                if(offset)
                    iods[i - offset] = iods[i];
            } /* end else */
        } /* end for */

        /* Read data from attribute */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_fetch(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, (unsigned)(attr_size - offset), iods, sgls, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_ATTR, H5E_READERROR, FAIL, "can't read data from attribute: %d", ret)
    } /* end if */
    else {
        daos_iod_t iod;
        daos_recx_t recx;
        daos_sg_list_t sgl;
        daos_iov_t sg_iov;
        size_t mem_type_size;
        size_t file_type_size;
        H5_daos_tconv_reuse_t reuse = H5_DAOS_TCONV_REUSE_NONE;
        hbool_t fill_bkg = FALSE;

        /* Check for type conversion */
        if(H5_daos_tconv_init(attr->type_id, &file_type_size, mem_type_id, &mem_type_size, (size_t)attr_size, &tconv_buf, &bkg_buf, &reuse, &fill_bkg) < 0)
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTINIT, FAIL, "can't initialize type conversion")

        /* Reuse buffer as appropriate */
        if(reuse == H5_DAOS_TCONV_REUSE_TCONV)
            tconv_buf = buf;
        else if(reuse == H5_DAOS_TCONV_REUSE_BKG)
            bkg_buf = buf;

        /* Fill background buffer if necessary */
        if(fill_bkg && (bkg_buf != buf))
            (void)memcpy(bkg_buf, buf, (size_t)attr_size * mem_type_size);

        /* Set up operation to read data */
        /* Create akey string (prefix "V-") */
        akey_len = strlen(attr->name) + 2;
        if(NULL == (akey = (char *)DV_malloc(akey_len + 1)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey")
        akey[0] = 'V';
        akey[1] = '-';
        (void)strcpy(akey + 2, attr->name);

        /* Set up recx */
        recx.rx_idx = (uint64_t)0;
        recx.rx_nr = attr_size;

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)akey, (daos_size_t)akey_len);
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_nr = 1u;
        iod.iod_recxs = &recx;
        iod.iod_size = (uint64_t)file_type_size;
        iod.iod_type = DAOS_IOD_ARRAY;

        /* Set up sgl */
        daos_iov_set(&sg_iov, tconv_buf ? tconv_buf : buf, (daos_size_t)(attr_size * (uint64_t)file_type_size));
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &sg_iov;

        /* Read data from attribute */
        if(0 != (ret = daos_obj_fetch(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_ATTR, H5E_READERROR, FAIL, "can't read data from attribute: %d", ret)

        /* Perform type conversion if necessary */
        if(tconv_buf) {
            /* Type conversion */
            if(H5Tconvert(attr->type_id, mem_type_id, attr_size, tconv_buf, bkg_buf, dxpl_id) < 0)
                D_GOTO_ERROR(H5E_ATTR, H5E_CANTCONVERT, FAIL, "can't perform type conversion")

            /* Copy to user's buffer if necessary */
            if(buf != tconv_buf)
                (void)memcpy(buf, tconv_buf, (size_t)attr_size * mem_type_size);
        } /* end if */
    } /* end else */

done:
    /* Free memory */
    akey = (char *)DV_free(akey);
    iods = (daos_iod_t *)DV_free(iods);
    sgls = (daos_sg_list_t *)DV_free(sgls);
    sg_iovs = (daos_iov_t *)DV_free(sg_iovs);
    if(tconv_buf && (tconv_buf != buf))
        DV_free(tconv_buf);
    if(bkg_buf && (bkg_buf != buf))
        DV_free(bkg_buf);

    if(akeys) {
        for(i = 0; i < attr_size; i++)
            DV_free(akeys[i]);
        DV_free(akeys);
    } /* end if */

    if(base_type_id != FAIL)
        if(H5Idec_ref(base_type_id) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close base type id")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_attribute_read() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_attribute_write
 *
 * Purpose:     Writes raw data from a buffer into an attribute.
 *
 * Return:      Success:        0
 *              Failure:        -1, attribute not written.
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_attribute_write(void *_attr, hid_t mem_type_id, const void *buf,
    hid_t DV_ATTR_UNUSED dxpl_id, void DV_ATTR_UNUSED **req)
{
    H5_daos_attr_t *attr = (H5_daos_attr_t *)_attr;
    int ndims;
    hsize_t dim[H5S_MAX_RANK];
    size_t akey_len;
    daos_key_t dkey;
    char *akey = NULL;
    uint8_t **akeys = NULL;
    daos_iod_t *iods = NULL;
    daos_sg_list_t *sgls = NULL;
    daos_iov_t *sg_iovs = NULL;
    char attr_key[] = H5_DAOS_ATTR_KEY;
    hid_t base_type_id = FAIL;
    size_t base_type_size = 0;
    uint64_t attr_size;
    void *tconv_buf = NULL;
    void *bkg_buf = NULL;
    H5T_class_t type_class;
    hbool_t is_vl = FALSE;
    htri_t is_vl_str = FALSE;
    int ret;
    uint64_t i;
    herr_t ret_value = SUCCEED;

    if(!buf)
        D_GOTO_ERROR(H5E_ATTR, H5E_BADVALUE, FAIL, "write buffer is NULL")

    /* Check for write access */
    if(!(attr->item.file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, FAIL, "no write intent on file")

    /* Get dataspace extent */
    if((ndims = H5Sget_simple_extent_ndims(attr->space_id)) < 0)
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get number of dimensions")
    if(ndims != H5Sget_simple_extent_dims(attr->space_id, dim, NULL))
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get dimensions")

    /* Calculate attribute size */
    attr_size = (uint64_t)1;
    for(i = 0; i < (uint64_t)ndims; i++)
        attr_size *= (uint64_t)dim[i];

    /* Set up dkey */
    daos_iov_set(&dkey, attr_key, (daos_size_t)(sizeof(attr_key) - 1));

    /* Check for vlen */
    if(H5T_NO_CLASS == (type_class = H5Tget_class(mem_type_id)))
        D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get datatype class")
    if(type_class == H5T_VLEN) {
        is_vl = TRUE;

        /* Calculate base type size */
        if((base_type_id = H5Tget_super(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get datatype base type")
        if(0 == (base_type_size = H5Tget_size(base_type_id)))
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get datatype base type size")
    } /* end if */
    else if(type_class == H5T_STRING) {
        /* check for vlen string */
        if((is_vl_str = H5Tis_variable_str(mem_type_id)) < 0)
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't check for variable length string")
        if(is_vl_str)
            is_vl = TRUE;
    } /* end if */

    /* Check for variable length */
    if(is_vl) {
        size_t akey_str_len;
        uint8_t *p;

        /* Calculate akey length */
        akey_str_len = strlen(attr->name) + 2;
        akey_len = akey_str_len + sizeof(uint64_t);

        /* Allocate array of akey pointers */
        if(NULL == (akeys = (uint8_t **)DV_calloc(attr_size * sizeof(uint8_t *))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey array")

        /* Allocate array of iods */
        if(NULL == (iods = (daos_iod_t *)DV_calloc(attr_size * sizeof(daos_iod_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for I/O descriptor array")

        /* Allocate array of sg_iovs */
        if(NULL == (sg_iovs = (daos_iov_t *)DV_malloc(attr_size * sizeof(daos_iov_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list")

        /* Allocate array of sgls */
        if(NULL == (sgls = (daos_sg_list_t *)DV_malloc(attr_size * sizeof(daos_sg_list_t))))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for scatter gather list array")

        /* Loop over elements */
        for(i = 0; i < attr_size; i++) {
            /* Create akey for this element */
            if(NULL == (akeys[i] = (uint8_t *)DV_malloc(akey_len)))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey")
            akeys[i][0] = 'V';
            akeys[i][1] = '-';
            (void)strcpy((char *)akeys[i] + 2, attr->name);
            p = akeys[i] + akey_str_len;
            UINT64ENCODE(p, i)

            /* Set up iod, determine size below.  Use "single" records of
             * varying size. */
            daos_iov_set(&iods[i].iod_name, (void *)akeys[i], (daos_size_t)akey_len);
            daos_csum_set(&iods[i].iod_kcsum, NULL, 0);
            iods[i].iod_nr = 1u;
            iods[i].iod_type = DAOS_IOD_SINGLE;

            /* Set up constant sgl info */
            sgls[i].sg_nr = 1;
            sgls[i].sg_nr_out = 0;
            sgls[i].sg_iovs = &sg_iovs[i];

            /* Check for vlen string */
            if(is_vl_str) {
                /* Find string for this element */
                char *elem = ((char * const *)buf)[i];

                /* Set string length in iod and buffer location in sgl.  If we
                 * are writing an empty string ("\0"), increase the size by one
                 * to differentiate it from NULL strings.  Note that this will
                 * cause the read buffer to be one byte longer than it needs to
                 * be in this case.  This should not cause any ill effects. */
                if(elem) {
                    iods[i].iod_size = (daos_size_t)strlen(elem);
                    if(iods[i].iod_size == 0)
                        iods[i].iod_size = 1;
                    daos_iov_set(&sg_iovs[i], (void *)elem, iods[i].iod_size);
                } /* end if */
                else {
                    iods[i].iod_size = 0;
                    daos_iov_set(&sg_iovs[i], NULL, 0);
                } /* end else */
            } /* end if */
            else {
                /* Standard vlen, find hvl_t struct for this element */
                const hvl_t *elem = &((const hvl_t *)buf)[i];

                assert(base_type_size > 0);

                /* Set buffer length in iod and buffer location in sgl */
                if(elem->len > 0) {
                    iods[i].iod_size = (daos_size_t)(elem->len * base_type_size);
                    daos_iov_set(&sg_iovs[i], (void *)elem->p, iods[i].iod_size);
                } /* end if */
                else {
                    iods[i].iod_size = 0;
                    daos_iov_set(&sg_iovs[i], NULL, 0);
                } /* end else */
            } /* end if */
        } /* end for */

        /* Write data to attribute */
        /* Note cast to unsigned reduces width to 32 bits.  Should eventually
         * check for overflow and iterate over 2^32 size blocks */
        if(0 != (ret = daos_obj_update(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, (unsigned)attr_size, iods, sgls, NULL /*event*/)))
            D_GOTO_ERROR(H5E_ATTR, H5E_WRITEERROR, FAIL, "can't write data to attribute: %d", ret)
    } /* end if */
    else {
        daos_iod_t iod;
        daos_recx_t recx;
        daos_sg_list_t sgl;
        daos_iov_t sg_iov;
        size_t mem_type_size;
        size_t file_type_size;
        hbool_t fill_bkg = FALSE;

        /* Check for type conversion */
        if(H5_daos_tconv_init(mem_type_id, &mem_type_size, attr->type_id, &file_type_size, (size_t)attr_size, &tconv_buf, &bkg_buf, NULL, &fill_bkg) < 0)
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTINIT, FAIL, "can't initialize type conversion")

        /* Set up operation to write data */
        /* Create akey string (prefix "V-") */
        akey_len = strlen(attr->name) + 2;
        if(NULL == (akey = (char *)DV_malloc(akey_len + 1)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akey")
        akey[0] = 'V';
        akey[1] = '-';
        (void)strcpy(akey + 2, attr->name);

        /* Set up recx */
        recx.rx_idx = (uint64_t)0;
        recx.rx_nr = attr_size;

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)akey, (daos_size_t)akey_len);
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_nr = 1u;
        iod.iod_recxs = &recx;
        iod.iod_size = (uint64_t)file_type_size;
        iod.iod_type = DAOS_IOD_ARRAY;

        /* Set up constant sgl info */
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &sg_iov;

        /* Check for type conversion */
        if(tconv_buf) {
            /* Check if we need to fill background buffer */
            if(fill_bkg) {
                assert(bkg_buf);

                /* Read data from attribute to background buffer */
                daos_iov_set(&sg_iov, bkg_buf, (daos_size_t)(attr_size * (uint64_t)file_type_size));

                if(0 != (ret = daos_obj_fetch(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*maps*/, NULL /*event*/)))
                    D_GOTO_ERROR(H5E_ATTR, H5E_READERROR, FAIL, "can't read data from attribute: %d", ret)
            } /* end if */

            /* Copy data to type conversion buffer */
            (void)memcpy(tconv_buf, buf, (size_t)attr_size * mem_type_size);

            /* Perform type conversion */
            if(H5Tconvert(mem_type_id, attr->type_id, attr_size, tconv_buf, bkg_buf, dxpl_id) < 0)
                D_GOTO_ERROR(H5E_ATTR, H5E_CANTCONVERT, FAIL, "can't perform type conversion")

            /* Set sgl to write from tconv_buf */
            daos_iov_set(&sg_iov, tconv_buf, (daos_size_t)(attr_size * (uint64_t)file_type_size));
        } /* end if */
        else
            /* Set sgl to write from buf */
            daos_iov_set(&sg_iov, (void *)buf, (daos_size_t)(attr_size * (uint64_t)file_type_size));

        /* Write data to attribute */
        if(0 != (ret = daos_obj_update(attr->parent->obj_oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_ATTR, H5E_WRITEERROR, FAIL, "can't write data to attribute: %d", ret)
    } /* end else */

done:
    /* Free memory */
    akey = (char *)DV_free(akey);
    iods = (daos_iod_t *)DV_free(iods);
    sgls = (daos_sg_list_t *)DV_free(sgls);
    sg_iovs = (daos_iov_t *)DV_free(sg_iovs);
    tconv_buf = DV_free(tconv_buf);
    bkg_buf = DV_free(bkg_buf);

    if(akeys) {
        for(i = 0; i < attr_size; i++)
            DV_free(akeys[i]);
        DV_free(akeys);
    } /* end if */

    if(base_type_id != FAIL)
        if(H5Idec_ref(base_type_id) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close base type id")

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_attribute_write() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_attribute_get
 *
 * Purpose:     Gets certain information about an attribute
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              May, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_attribute_get(void *_item, H5VL_attr_get_t get_type,
    hid_t DV_ATTR_UNUSED dxpl_id, void DV_ATTR_UNUSED **req, va_list arguments)
{
    herr_t  ret_value = SUCCEED;    /* Return value */

    switch (get_type) {
        /* H5Aget_space */
        case H5VL_ATTR_GET_SPACE:
            {
                hid_t *ret_id = va_arg(arguments, hid_t *);
                H5_daos_attr_t *attr = (H5_daos_attr_t *)_item;

                /* Retrieve the attribute's dataspace */
                if((*ret_id = H5Scopy(attr->space_id)) < 0)
                    D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get dataspace ID of dataset");
                break;
            } /* end block */
        /* H5Aget_type */
        case H5VL_ATTR_GET_TYPE:
            {
                hid_t *ret_id = va_arg(arguments, hid_t *);
                H5_daos_attr_t *attr = (H5_daos_attr_t *)_item;

                /* Retrieve the attribute's datatype */
                if((*ret_id = H5Tcopy(attr->type_id)) < 0)
                    D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get datatype ID of dataset")
                break;
            } /* end block */
        /* H5Aget_create_plist */
        case H5VL_ATTR_GET_ACPL:
            {
                hid_t *ret_id = va_arg(arguments, hid_t *);
                /*H5_daos_attr_t *attr = (H5_daos_attr_t *)_item;*/

                /* Retrieve the file's access property list */
                if((*ret_id = H5Pcopy(H5P_ATTRIBUTE_CREATE_DEFAULT)) < 0)
                    D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get attr creation property list");
                break;
            } /* end block */
        /* H5Aget_name */
        case H5VL_ATTR_GET_NAME:
            {
                H5VL_loc_params_t *loc_params = va_arg(arguments, H5VL_loc_params_t *);
                size_t buf_size = va_arg(arguments, size_t);
                char *buf = va_arg(arguments, char *);
                ssize_t *ret_val = va_arg(arguments, ssize_t *);
                H5_daos_attr_t *attr = (H5_daos_attr_t *)_item;

                if(H5VL_OBJECT_BY_SELF == loc_params->type) {
                    size_t copy_len;
                    size_t nbytes;

                    nbytes = strlen(attr->name);
                    assert((ssize_t)nbytes >= 0); /*overflow, pretty unlikely --rpm*/

                    /* compute the string length which will fit into the user's buffer */
                    copy_len = MIN(buf_size - 1, nbytes);

                    /* Copy all/some of the name */
                    if(buf && copy_len > 0) {
                        memcpy(buf, attr->name, copy_len);

                        /* Terminate the string */
                        buf[copy_len]='\0';
                    } /* end if */
                    *ret_val = (ssize_t)nbytes;
                } /* end if */
                else if(H5VL_OBJECT_BY_IDX == loc_params->type) {
                    *ret_val = -1;
                    D_GOTO_ERROR(H5E_ATTR, H5E_UNSUPPORTED, FAIL, "get attribute name by index unsupported");
                } /* end else */
                break;
            } /* end block */
        /* H5Aget_info */
        case H5VL_ATTR_GET_INFO:
        case H5VL_ATTR_GET_STORAGE_SIZE:
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "can't get this type of information from attr")
    } /* end switch */

done:
    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_attribute_get() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_attribute_specific
 *
 * Purpose:     Specific operations with attributes
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_attribute_specific(void *_item, const H5VL_loc_params_t *loc_params,
    H5VL_attr_specific_t specific_type, hid_t dxpl_id, void **req,
    va_list arguments)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_obj_t *target_obj = NULL;
    hid_t target_obj_id = FAIL;
    char *akey_buf = NULL;
    size_t akey_buf_len = 0;
    H5_daos_attr_t *attr = NULL;
    int ret;
    herr_t ret_value = SUCCEED;    /* Return value */
    
    /* Determine attribute object */
    if(loc_params->type == H5VL_OBJECT_BY_SELF) {
        /* Use item as attribute parent object, or the root group if item is a
         * file */
        if(item->type == H5I_FILE)
            target_obj = (H5_daos_obj_t *)((H5_daos_file_t *)item)->root_grp;
        else
            target_obj = (H5_daos_obj_t *)item;
        target_obj->item.rc++;
    } /* end if */
    else if(loc_params->type == H5VL_OBJECT_BY_NAME) {
        /* Open target_obj */
        if(NULL == (target_obj = (H5_daos_obj_t *)H5_daos_object_open(item, loc_params, NULL, dxpl_id, req)))
            D_GOTO_ERROR(H5E_ATTR, H5E_CANTOPENOBJ, FAIL, "can't open object for attribute")
    } /* end else */
    else
        D_GOTO_ERROR(H5E_ATTR, H5E_UNSUPPORTED, FAIL, "unsupported attribute operation location parameters type")

    switch (specific_type) {
        /* H5Aexists */
        case H5VL_ATTR_DELETE:
        case H5VL_ATTR_EXISTS:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "unsupported specific operation")
#ifdef DV_HAVE_ATTR_ITERATION
        case H5VL_ATTR_ITER:
            {
                H5_index_t DV_ATTR_UNUSED idx_type = (H5_index_t)va_arg(arguments, int);
                H5_iter_order_t order = (H5_iter_order_t)va_arg(arguments, int);
                hsize_t *idx = va_arg(arguments, hsize_t *);
                H5A_operator2_t op = va_arg(arguments, H5A_operator2_t);
                void *op_data = va_arg(arguments, void *);
                daos_anchor_t anchor;
                char attr_key[] = H5_DAOS_ATTR_KEY;
                daos_key_t dkey;
                uint32_t nr;
                daos_key_desc_t kds[H5_DAOS_ITER_LEN];
                daos_sg_list_t sgl;
                daos_iov_t sg_iov;
                H5VL_loc_params_t sub_loc_params;
                H5A_info_t ainfo;
                herr_t op_ret;
                char tmp_char;
                char *p;
                uint32_t i;

                /* Iteration restart not supported */
                if(idx && (*idx != 0))
                    D_GOTO_ERROR(H5E_ATTR, H5E_UNSUPPORTED, FAIL, "iteration restart not supported (must start from 0)")

                /* Ordered iteration not supported */
                if(order != H5_ITER_NATIVE)
                    D_GOTO_ERROR(H5E_ATTR, H5E_UNSUPPORTED, FAIL, "ordered iteration not supported (order must be H5_ITER_NATIVE)")

                /* Initialize sub_loc_params */
                sub_loc_params.obj_type = target_obj->item.type;
                sub_loc_params.type = H5VL_OBJECT_BY_SELF;

                /* Initialize const ainfo info */
                ainfo.corder_valid = FALSE;
                ainfo.corder = 0;
                ainfo.cset = H5T_CSET_ASCII;

                /* Register id for target_obj */
                if((target_obj_id = H5VLregister(target_obj->item.type, target_obj, H5_DAOS_g)) < 0)
                    D_GOTO_ERROR(H5E_ATOM, H5E_CANTREGISTER, FAIL, "unable to atomize object handle")

                /* Initialize anchor */
                memset(&anchor, 0, sizeof(anchor));

                /* Set up dkey */
                daos_iov_set(&dkey, attr_key, (daos_size_t)(sizeof(attr_key) - 1));

                /* Allocate akey_buf */
                if(NULL == (akey_buf = (char *)DV_malloc(H5_DAOS_ITER_SIZE_INIT)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akeys")
                akey_buf_len = H5_DAOS_ITER_SIZE_INIT;

                /* Set up sgl.  Report size as 1 less than buffer size so we
                 * always have room for a null terminator. */
                daos_iov_set(&sg_iov, akey_buf, (daos_size_t)(akey_buf_len - 1));
                sgl.sg_nr = 1;
                sgl.sg_nr_out = 0;
                sgl.sg_iovs = &sg_iov;

                /* Loop to retrieve keys and make callbacks */
                do {
                    /* Loop to retrieve keys (exit as soon as we get at least 1
                     * key) */
                    do {
                        /* Reset nr */
                        nr = H5_DAOS_ITER_LEN;

                        /* Ask daos for a list of akeys, break out if we succeed
                         */
                        if(0 == (ret = daos_obj_list_akey(target_obj->obj_oh, DAOS_TX_NONE, &dkey, &nr, kds, &sgl, &anchor, NULL /*event*/)))
                            break;

                        /* Call failed, if the buffer is too small double it and
                         * try again, otherwise fail */
                        if(ret == -DER_KEY2BIG) {
                            /* Allocate larger buffer */
                            DV_free(akey_buf);
                            akey_buf_len *= 2;
                            if(NULL == (akey_buf = (char *)DV_malloc(akey_buf_len)))
                                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "can't allocate buffer for akeys")

                            /* Update sgl */
                            daos_iov_set(&sg_iov, akey_buf, (daos_size_t)(akey_buf_len - 1));
                        } /* end if */
                        else
                            D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't retrieve attributes: %d", ret)
                    } while(1);

                    /* Loop over returned akeys */
                    p = akey_buf;
                    op_ret = 0;
                    for(i = 0; (i < nr) && (op_ret == 0); i++) {
                        /* Check for invalid key */
                        if(kds[i].kd_key_len < 3)
                            D_GOTO_ERROR(H5E_ATTR, H5E_CANTDECODE, FAIL, "attribute akey too short")
                        if(p[1] != '-')
                            D_GOTO_ERROR(H5E_ATTR, H5E_CANTDECODE, FAIL, "invalid attribute akey format")

                        /* Only do callbacks for "S-" (dataspace) keys, to avoid
                         * duplication */
                        if(p[0] == 'S') {
                            hssize_t npoints;
                            size_t type_size;

                            /* Add null terminator temporarily */
                            tmp_char = p[kds[i].kd_key_len];
                            p[kds[i].kd_key_len] = '\0';

                            /* Open attribute */
                            if(NULL == (attr = (H5_daos_attr_t *)H5_daos_attribute_open(target_obj, &sub_loc_params, &p[2], H5P_DEFAULT, dxpl_id, req)))
                                D_GOTO_ERROR(H5E_ATTR, H5E_CANTOPENOBJ, FAIL, "can't open attribute")

                            /* Get number of elements in attribute */
                            if((npoints = (H5Sget_simple_extent_npoints(attr->space_id))) < 0)
                                D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get number of elements in attribute")

                            /* Get attribute datatype size */
                            if(0 == (type_size = H5Tget_size(attr->type_id)))
                                D_GOTO_ERROR(H5E_ATTR, H5E_CANTGET, FAIL, "can't get attribute datatype size")

                            /* Set attribute size */
                            ainfo.data_size = (hsize_t)npoints * (hsize_t)type_size;

                            /* Make callback */
                            if((op_ret = op(target_obj_id, &p[2], &ainfo, op_data)) < 0)
                                D_GOTO_ERROR(H5E_ATTR, H5E_BADITER, op_ret, "operator function returned failure")

                            /* Close attribute */
                            if(H5_daos_attribute_close(attr, dxpl_id, req) < 0)
                                D_GOTO_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close attribute")

                            /* Replace null terminator */
                            p[kds[i].kd_key_len] = tmp_char;

                            /* Advance idx */
                            if(idx)
                                (*idx)++;
                        } /* end if */

                        /* Advance to next akey */
                        p += kds[i].kd_key_len + kds[i].kd_csum_len;
                    } /* end for */
                } while(!daos_anchor_is_eof(&anchor) && (op_ret == 0));

                /* Set return value */
                ret_value = op_ret;

                break;
            } /* end block */
#endif

        case H5VL_ATTR_RENAME:
            D_GOTO_ERROR(H5E_VOL, H5E_UNSUPPORTED, FAIL, "unsupported specific operation")
        default:
            D_GOTO_ERROR(H5E_VOL, H5E_BADVALUE, FAIL, "invalid specific operation")
    } /* end switch */

done:
    if(target_obj_id != FAIL) {
        if(H5Idec_ref(target_obj_id) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close object id")
        target_obj_id = FAIL;
        target_obj = NULL;
    } /* end if */
    else if(target_obj) {
        if(H5_daos_object_close(target_obj, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close object")
        target_obj = NULL;
    } /* end else */
    if(attr) {
        if(H5_daos_attribute_close(attr, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close attribute")
        attr = NULL;
    } /* end if */
    akey_buf = (char *)DV_free(akey_buf);

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_attribute_specific() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_attribute_close
 *
 * Purpose:     Closes a DAOS HDF5 attribute.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              February, 2017
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_attribute_close(void *_attr, hid_t dxpl_id, void **req)
{
    H5_daos_attr_t *attr = (H5_daos_attr_t *)_attr;
    herr_t ret_value = SUCCEED;

    assert(attr);

    if(--attr->item.rc == 0) {
        /* Free attribute data structures */
        if(attr->parent && H5_daos_object_close(attr->parent, dxpl_id, req))
            D_DONE_ERROR(H5E_ATTR, H5E_CLOSEERROR, FAIL, "can't close parent object")
        DV_free(attr->name);
        if(attr->type_id != FAIL && H5Idec_ref(attr->type_id) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CANTDEC, FAIL, "failed to close datatype")
        if(attr->space_id != FAIL && H5Idec_ref(attr->space_id) < 0)
            D_DONE_ERROR(H5E_ATTR, H5E_CANTDEC, FAIL, "failed to close dataspace")
        attr = H5FL_FREE(H5_daos_attr_t, attr);
    } /* end if */

    PRINT_ERROR_STACK

    D_FUNC_LEAVE
} /* end H5_daos_attribute_close() */

#ifdef DV_HAVE_MAP

/*-------------------------------------------------------------------------
 * Function:    H5_daos_map_create
 *
 * Purpose:     Sends a request to DAOS to create a map
 *
 * Return:      Success:        map object. 
 *              Failure:        NULL
 *
 *-------------------------------------------------------------------------
 */
void *
H5_daos_map_create(void *_item, H5VL_loc_params_t DV_ATTR_UNUSED *loc_params,
    const char *name, hid_t ktype_id, hid_t vtype_id,
    hid_t DV_ATTR_UNUSED mcpl_id, hid_t mapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_map_t *map = NULL;
    H5_daos_group_t *target_grp = NULL;
    void *ktype_buf = NULL;
    void *vtype_buf = NULL;
    hbool_t collective = item->file->collective;
    int ret;
    void *ret_value = NULL;

    /* Check for write access */
    if(!(item->file->flags & H5F_ACC_RDWR))
        D_GOTO_ERROR(H5E_FILE, H5E_BADVALUE, NULL, "no write intent on file")
 
    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(mapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, NULL, "can't get collective access property")

    /* Allocate the map object that is returned to the user */
    if(NULL == (map = H5FL_CALLOC(H5_daos_map_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS map struct")
    map->obj.item.type = H5I_MAP;
    map->obj.item.file = item->file;
    map->obj.item.rc = 1;
    map->obj.obj_oh = DAOS_HDL_INVAL;
    map->ktype_id = FAIL;
    map->vtype_id = FAIL;

    /* Generate map oid */
    H5_daos_oid_encode(&map->obj.oid, item->file->max_oid + (uint64_t)1, H5I_MAP);

    /* Create map and write metadata if this process should */
    if(!collective || (item->file->my_rank == 0)) {
        const char *target_name = NULL;
        H5_daos_link_val_t link_val;
        daos_key_t dkey;
        daos_iod_t iod[2];
        daos_sg_list_t sgl[2];
        daos_iov_t sg_iov[2];
        size_t ktype_size = 0;
        size_t vtype_size = 0;
        char int_md_key[] = H5_DAOS_INT_MD_KEY;
        char ktype_key[] = H5_DAOS_KTYPE_KEY;
        char vtype_key[] = H5_DAOS_VTYPE_KEY;

        /* Traverse the path */
        if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id,
                req, &target_name, NULL, NULL)))
            D_GOTO_ERROR(H5E_MAP, H5E_BADITER, NULL, "can't traverse path")

        /* Create map */
        /* Update max_oid */
        item->file->max_oid = H5_daos_oid_to_idx(map->obj.oid);

        /* Write max OID */
        if(H5_daos_write_max_oid(item->file) < 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTINIT, NULL, "can't write max OID")

        /* Open map */
        if(0 != (ret = daos_obj_open(item->file->coh, map->obj.oid, DAOS_OO_RW, &map->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTOPENOBJ, NULL, "can't open map: %d", ret)

        /* Encode datatypes */
        if(H5Tencode(ktype_id, NULL, &ktype_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of datatype")
        if(NULL == (ktype_buf = DV_malloc(ktype_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized datatype")
        if(H5Tencode(ktype_id, ktype_buf, &ktype_size) < 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTENCODE, NULL, "can't serialize datatype")

        if(H5Tencode(vtype_id, NULL, &vtype_size) < 0)
            D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "can't determine serialized length of datatype")
        if(NULL == (vtype_buf = DV_malloc(vtype_size)))
            D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate buffer for serialized datatype")
        if(H5Tencode(vtype_id, vtype_buf, &vtype_size) < 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTENCODE, NULL, "can't serialize datatype")

        /* Eventually we will want to store the MCPL in the file, and hold
         * copies of the MCP and MAPL in memory.  To do this look at the dataset
         * and group code for examples.  -NAF */

        /* Set up operation to write datatypes to map */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, (void *)ktype_key, (daos_size_t)(sizeof(ktype_key) - 1));
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = (uint64_t)ktype_size;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, (void *)vtype_key, (daos_size_t)(sizeof(vtype_key) - 1));
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = (uint64_t)vtype_size;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        /* Set up sgl */
        daos_iov_set(&sg_iov[0], ktype_buf, (daos_size_t)ktype_size);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        daos_iov_set(&sg_iov[1], vtype_buf, (daos_size_t)vtype_size);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];

        /* Write internal metadata to map */
        if(0 != (ret = daos_obj_update(map->obj.obj_oh, DAOS_TX_NONE, &dkey, 2, iod, sgl, NULL /*event*/)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTINIT, NULL, "can't write metadata to map: %d", ret)

        /* Create link to map */
        link_val.type = H5L_TYPE_HARD;
        link_val.target.hard = map->obj.oid;
        if(H5_daos_link_write(target_grp, target_name, strlen(target_name), &link_val) < 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTINIT, NULL, "can't create link to map")
    } /* end if */
    else {
        /* Update max_oid */
        item->file->max_oid = map->obj.oid.lo;

        /* Open map */
        if(0 != (ret = daos_obj_open(item->file->coh, map->obj.oid, DAOS_OO_RW, &map->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTOPENOBJ, NULL, "can't open map: %d", ret)
    } /* end else */

    /* Finish setting up map struct */
    if((map->ktype_id = H5Tcopy(ktype_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy datatype")
    if((map->vtype_id = H5Tcopy(vtype_id)) < 0)
        D_GOTO_ERROR(H5E_SYM, H5E_CANTCOPY, NULL, "failed to copy datatype")

    /* Set return value */
    ret_value = (void *)map;

done:
    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_MAP, H5E_CLOSEERROR, NULL, "can't close group")

    /* Cleanup on failure */
    /* Destroy DAOS object if created before failure DSINC */
    if(NULL == ret_value)
        /* Close map */
        if(map && H5_daos_map_close(map, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_MAP, H5E_CLOSEERROR, NULL, "can't close map");

    /* Free memory */
    ktype_buf = DV_free(ktype_buf);
    vtype_buf = DV_free(vtype_buf);

    D_FUNC_LEAVE
} /* end H5_daos_map_create() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_map_open
 *
 * Purpose:     Sends a request to DAOS to open a map
 *
 * Return:      Success:        map object. 
 *              Failure:        NULL
 *
 *-------------------------------------------------------------------------
 */
void *
H5_daos_map_open(void *_item, H5VL_loc_params_t *loc_params, const char *name,
    hid_t mapl_id, hid_t dxpl_id, void **req)
{
    H5_daos_item_t *item = (H5_daos_item_t *)_item;
    H5_daos_map_t *map = NULL;
    H5_daos_group_t *target_grp = NULL;
    const char *target_name = NULL;
    daos_key_t dkey;
    daos_iod_t iod[2];
    daos_sg_list_t sgl[2];
    daos_iov_t sg_iov[2];
    uint64_t ktype_len = 0;
    uint64_t vtype_len = 0;
    uint64_t tot_len;
    uint8_t minfo_buf_static[H5_DAOS_DINFO_BUF_SIZE];
    uint8_t *minfo_buf_dyn = NULL;
    uint8_t *minfo_buf = minfo_buf_static;
    char int_md_key[] = H5_DAOS_INT_MD_KEY;
    char ktype_key[] = H5_DAOS_KTYPE_KEY;
    char vtype_key[] = H5_DAOS_VTYPE_KEY;
    uint8_t *p;
    hbool_t collective = item->file->collective;
    hbool_t must_bcast = FALSE;
    int ret;
    void *ret_value = NULL;
 
    /* Check for collective access, if not already set by the file */
    if(!collective)
        if(H5Pget_all_coll_metadata_ops(mapl_id, &collective) < 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, NULL, "can't get collective access property")

    /* Allocate the map object that is returned to the user */
    if(NULL == (map = H5FL_CALLOC(H5_daos_map_t)))
        D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate DAOS map struct")
    map->obj.item.type = H5I_MAP;
    map->obj.item.file = item->file;
    map->obj.item.rc = 1;
    map->obj.obj_oh = DAOS_HDL_INVAL;
    map->ktype_id = FAIL;
    map->vtype_id = FAIL;

    /* Check if we're actually opening the group or just receiving the map
     * info from the leader */
    if(!collective || (item->file->my_rank == 0)) {
        if(collective && (item->file->num_procs > 1))
            must_bcast = TRUE;

        /* Check for open by address */
        if(H5VL_OBJECT_BY_ADDR == loc_params.type) {
            /* Generate oid from address */
            H5_daos_oid_generate(&map->obj.oid, (uint64_t)loc_params.loc_data.loc_by_addr.addr, H5I_MAP);
        } /* end if */
        else {
            /* Open using name parameter */
            /* Traverse the path */
            if(NULL == (target_grp = H5_daos_group_traverse(item, name, dxpl_id, req, &target_name, NULL, NULL)))
                D_GOTO_ERROR(H5E_MAP, H5E_BADITER, NULL, "can't traverse path")

            /* Follow link to map */
            if(H5_daos_link_follow(target_grp, target_name, strlen(target_name), dxpl_id, req, &map->obj.oid) < 0)
                D_GOTO_ERROR(H5E_MAP, H5E_CANTINIT, NULL, "can't follow link to map")
        } /* end else */

        /* Open map */
        if(0 != (ret = daos_obj_open(item->file->coh, map->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &map->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTOPENOBJ, NULL, "can't open map: %d", ret)

        /* Set up operation to read datatype sizes from map */
        /* Set up dkey */
        daos_iov_set(&dkey, int_md_key, (daos_size_t)(sizeof(int_md_key) - 1));

        /* Set up iod */
        memset(iod, 0, sizeof(iod));
        daos_iov_set(&iod[0].iod_name, (void *)ktype_key, (daos_size_t)(sizeof(ktype_key) - 1));
        daos_csum_set(&iod[0].iod_kcsum, NULL, 0);
        iod[0].iod_nr = 1u;
        iod[0].iod_size = DAOS_REC_ANY;
        iod[0].iod_type = DAOS_IOD_SINGLE;

        daos_iov_set(&iod[1].iod_name, (void *)vtype_key, (daos_size_t)(sizeof(vtype_key) - 1));
        daos_csum_set(&iod[1].iod_kcsum, NULL, 0);
        iod[1].iod_nr = 1u;
        iod[1].iod_size = DAOS_REC_ANY;
        iod[1].iod_type = DAOS_IOD_SINGLE;

        /* Read internal metadata sizes from map */
        if(0 != (ret = daos_obj_fetch(map->obj.obj_oh, DAOS_TX_NONE, &dkey, 2, iod, NULL,
                      NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTDECODE, NULL, "can't read metadata sizes from map: %d", ret)

        /* Check for metadata not found */
        if((iod[0].iod_size == (uint64_t)0) || (iod[1].iod_size == (uint64_t)0))
            D_GOTO_ERROR(H5E_MAP, H5E_NOTFOUND, NULL, "internal metadata not found");

        /* Compute map info buffer size */
        ktype_len = iod[0].iod_size;
        vtype_len = iod[1].iod_size;
        tot_len = ktype_len + vtype_len;

        /* Allocate map info buffer if necessary */
        if((tot_len + (4 * sizeof(uint64_t))) > sizeof(minfo_buf_static)) {
            if(NULL == (minfo_buf_dyn = (uint8_t *)DV_malloc(tot_len + (4 * sizeof(uint64_t)))))
                D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate map info buffer")
            minfo_buf = minfo_buf_dyn;
        } /* end if */

        /* Set up sgl */
        p = minfo_buf + (4 * sizeof(uint64_t));
        daos_iov_set(&sg_iov[0], p, (daos_size_t)ktype_len);
        sgl[0].sg_nr = 1;
        sgl[0].sg_nr_out = 0;
        sgl[0].sg_iovs = &sg_iov[0];
        p += ktype_len;
        daos_iov_set(&sg_iov[1], p, (daos_size_t)vtype_len);
        sgl[1].sg_nr = 1;
        sgl[1].sg_nr_out = 0;
        sgl[1].sg_iovs = &sg_iov[1];

        /* Read internal metadata from map */
        if(0 != (ret = daos_obj_fetch(map->obj.obj_oh, DAOS_TX_NONE, &dkey, 2, iod, sgl, NULL /*maps*/, NULL /*event*/)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTDECODE, NULL, "can't read metadata from map: %d", ret)

        /* Broadcast map info if there are other processes that need it */
        if(collective && (item->file->num_procs > 1)) {
            assert(minfo_buf);
            assert(sizeof(minfo_buf_static) >= 4 * sizeof(uint64_t));

            /* Encode oid */
            p = minfo_buf;
            UINT64ENCODE(p, map->obj.oid.lo)
            UINT64ENCODE(p, map->obj.oid.hi)

            /* Encode serialized info lengths */
            UINT64ENCODE(p, ktype_len)
            UINT64ENCODE(p, vtype_len)

            /* MPI_Bcast minfo_buf */
            if(MPI_SUCCESS != MPI_Bcast((char *)minfo_buf, sizeof(minfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_MAP, H5E_MPI, NULL, "can't bcast map info");

            /* Need a second bcast if it did not fit in the receivers' static
             * buffer */
            if(tot_len + (4 * sizeof(uint64_t)) > sizeof(minfo_buf_static))
                if(MPI_SUCCESS != MPI_Bcast((char *)p, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                    D_GOTO_ERROR(H5E_MAP, H5E_MPI, NULL, "can't bcast map info (second bcast)")
        } /* end if */
        else
            p = minfo_buf + (4 * sizeof(uint64_t));
    } /* end if */
    else {
        /* Receive map info */
        if(MPI_SUCCESS != MPI_Bcast((char *)minfo_buf, sizeof(minfo_buf_static), MPI_BYTE, 0, item->file->comm))
            D_GOTO_ERROR(H5E_MAP, H5E_MPI, NULL, "can't bcast map info")

        /* Decode oid */
        p = minfo_buf_static;
        UINT64DECODE(p, map->obj.oid.lo)
        UINT64DECODE(p, map->obj.oid.hi)

        /* Decode serialized info lengths */
        UINT64DECODE(p, ktype_len)
        UINT64DECODE(p, vtype_len)
        tot_len = ktype_len + vtype_len;

        /* Check for type_len set to 0 - indicates failure */
        if(ktype_len == 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTINIT, NULL, "lead process failed to open map")

        /* Check if we need to perform another bcast */
        if(tot_len + (4 * sizeof(uint64_t)) > sizeof(minfo_buf_static)) {
            /* Allocate a dynamic buffer if necessary */
            if(tot_len > sizeof(minfo_buf_static)) {
                if(NULL == (minfo_buf_dyn = (uint8_t *)DV_malloc(tot_len)))
                    D_GOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate space for map info")
                minfo_buf = minfo_buf_dyn;
            } /* end if */

            /* Receive map info */
            if(MPI_SUCCESS != MPI_Bcast((char *)minfo_buf, (int)tot_len, MPI_BYTE, 0, item->file->comm))
                D_GOTO_ERROR(H5E_MAP, H5E_MPI, NULL, "can't bcast map info (second bcast)")

            p = minfo_buf;
        } /* end if */

        /* Open map */
        if(0 != (ret = daos_obj_open(item->file->coh, map->obj.oid, item->file->flags & H5F_ACC_RDWR ? DAOS_COO_RW : DAOS_COO_RO, &map->obj.obj_oh, NULL /*event*/)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTOPENOBJ, NULL, "can't open map: %d", ret)
    } /* end else */

    /* Decode datatype, dataspace, and DCPL */
    if((map->ktype_id = H5Tdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")
    p += ktype_len;
    if((map->vtype_id = H5Tdecode(p)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTDECODE, NULL, "can't deserialize datatype")

    /* Set return value */
    ret_value = (void *)map;

done:
    /* Cleanup on failure */
    if(NULL == ret_value) {
        /* Bcast minfo_buf as '0' if necessary - this will trigger failures in
         * in other processes so we do not need to do the second bcast. */
        if(must_bcast) {
            memset(minfo_buf_static, 0, sizeof(minfo_buf_static));
            if(MPI_SUCCESS != MPI_Bcast(minfo_buf_static, sizeof(minfo_buf_static), MPI_BYTE, 0, item->file->comm))
                D_DONE_ERROR(H5E_MAP, H5E_MPI, NULL, "can't bcast empty map info")
        } /* end if */

        /* Close map */
        if(map && H5_daos_map_close(map, dxpl_id, req) < 0)
            D_DONE_ERROR(H5E_MAP, H5E_CLOSEERROR, NULL, "can't close map")
    } /* end if */

    /* Close target group */
    if(target_grp && H5_daos_group_close(target_grp, dxpl_id, req) < 0)
        D_DONE_ERROR(H5E_MAP, H5E_CLOSEERROR, NULL, "can't close group")

    /* Free memory */
    minfo_buf_dyn = (uint8_t *)DV_free(minfo_buf_dyn);

    D_FUNC_LEAVE
} /* end H5_daos_map_open() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_map_get_size
 *
 * Purpose:     Retrieves the size of a Key or Value binary 
 *              buffer given its datatype and buffer contents.
 *
 * Return:      Success:        SUCCEED 
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_map_get_size(hid_t type_id, const void *buf,
    /*out*/uint64_t DV_ATTR_UNUSED *checksum,  /*out*/size_t *size,
    /*out*/H5T_class_t *ret_class)
{
    size_t buf_size = 0;
    H5T_t *dt = NULL;
    H5T_class_t dt_class;
    herr_t ret_value = SUCCEED;

    if(NULL == (dt = (H5T_t *)H5Iobject_verify(type_id, H5I_DATATYPE)))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, H5T_NO_CLASS, "not a datatype")

    dt_class = H5T_get_class(dt, FALSE);

    switch(dt_class) {
        case H5T_STRING:
            /* If this is a variable length string, get the size using strlen(). */
            if(H5T_is_variable_str(dt)) {
                buf_size = strlen((const char*)buf) + 1;

                break;
            }
        case H5T_INTEGER:
        case H5T_FLOAT:
        case H5T_TIME:
        case H5T_BITFIELD:
        case H5T_OPAQUE:
        case H5T_ENUM:
        case H5T_ARRAY:
        case H5T_NO_CLASS:
        case H5T_REFERENCE:
        case H5T_NCLASSES:
        case H5T_COMPOUND:
            /* Data is not variable length, so use H5Tget_size() */
            /* MSC - This is not correct. Compound/Array can contian
               VL datatypes, but for now we don't support that. Need
               to check for that too */
            buf_size = H5T_get_size(dt);

            break;

            /* If this is a variable length datatype, iterate over it */
        case H5T_VLEN:
            {
                H5T_t *super = NULL;
                const hvl_t *vl;

                vl = (const hvl_t *)buf;

                if(NULL == (super = H5T_get_super(dt)))
                    D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "invalid super type of VL type");

                buf_size = H5T_get_size(super) * vl->len;
                H5T_close(super);
                break;
            } /* end block */
        default:
            D_GOTO_ERROR(H5E_ARGS, H5E_CANTINIT, FAIL, "unsupported datatype");
    } /* end switch */

    *size = buf_size;
    if(ret_class)
        *ret_class = dt_class;

done:
    D_FUNC_LEAVE
} /* end H5_daos_map_get_size */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_map_dtype_info
 *
 * Purpose:     Retrieves information about the datatype of Map Key or
 *              value datatype, whether it's VL or not. If it is not VL
 *              return the size.
 *
 * Return:      Success:        SUCCEED 
 *              Failure:        Negative
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5_daos_map_dtype_info(hid_t type_id, hbool_t *is_vl, size_t *size,
    H5T_class_t *cls)
{
    size_t buf_size = 0;
    H5T_t *dt = NULL;
    H5T_class_t dt_class;
    herr_t ret_value = SUCCEED;

    if(NULL == (dt = (H5T_t *)H5Iobject_verify(type_id, H5I_DATATYPE)))
        D_GOTO_ERROR(H5E_ARGS, H5E_BADTYPE, H5T_NO_CLASS, "not a datatype")

    dt_class = H5T_get_class(dt, FALSE);

    switch(dt_class) {
        case H5T_STRING:
            /* If this is a variable length string, get the size using strlen(). */
            if(H5T_is_variable_str(dt)) {
                *is_vl = TRUE;
                break;
            }
        case H5T_INTEGER:
        case H5T_FLOAT:
        case H5T_TIME:
        case H5T_BITFIELD:
        case H5T_OPAQUE:
        case H5T_ENUM:
        case H5T_ARRAY:
        case H5T_NO_CLASS:
        case H5T_REFERENCE:
        case H5T_NCLASSES:
        case H5T_COMPOUND:
            /* Data is not variable length, so use H5Tget_size() */
            /* MSC - This is not correct. Compound/Array can contian
               VL datatypes, but for now we don't support that. Need
               to check for that too */
            buf_size = H5T_get_size(dt);
            *is_vl = FALSE;
            break;

            /* If this is a variable length datatype, iterate over it */
        case H5T_VLEN:
            *is_vl = TRUE;
            break;
        default:
            D_GOTO_ERROR(H5E_ARGS, H5E_CANTINIT, FAIL, "unsupported datatype");
    }

    if(size)
        *size = buf_size;
    if(cls)
        *cls = dt_class;
done:
    D_FUNC_LEAVE
} /* end H5_daos_map_dtype_info */


herr_t 
H5_daos_map_set(void *_map, hid_t key_mem_type_id, const void *key,
    hid_t val_mem_type_id, const void *value, hid_t DV_ATTR_UNUSED dxpl_id,
    void DV_ATTR_UNUSED **req)
{
    H5_daos_map_t *map = (H5_daos_map_t *)_map;
    size_t key_size, val_size;
    char const_akey[] = H5_DAOS_MAP_KEY;
    daos_key_t dkey;
    daos_iod_t iod;
    daos_sg_list_t sgl;
    daos_iov_t sg_iov;
    H5T_class_t cls;
    herr_t ret_value = SUCCEED;

    /* get the key size and checksum from the provdied key datatype & buffer */
    if(H5_daos_map_get_size(key_mem_type_id, key, NULL, &key_size, NULL) < 0)
        D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "can't get key size");

    /* get the val size and checksum from the provdied val datatype & buffer */
    if(H5_daos_map_get_size(val_mem_type_id, value, NULL, &val_size, &cls) < 0)
        D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "can't get val size");

        /* Set up dkey */
        daos_iov_set(&dkey, (void *)key, (daos_size_t)key_size);

        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, (void *)const_akey, (daos_size_t)(sizeof(const_akey) - 1));
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_nr = 1u;
        iod.iod_size = (daos_size_t)val_size;
        iod.iod_type = DAOS_IOD_SINGLE;

    /* Set up sgl */
    if (H5T_VLEN == cls) {
        hvl_t *vl_buf = (hvl_t *)value;

        daos_iov_set(&sg_iov, (void *)vl_buf->p, (daos_size_t)val_size);
    }
    else {
        daos_iov_set(&sg_iov, (void *)value, (daos_size_t)val_size);
    }

    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &sg_iov;

        if(0 != (ret_value = daos_obj_update(map->obj.obj_oh,
                         DAOS_TX_NONE, &dkey,
                         1, &iod, &sgl, NULL)))
        D_GOTO_ERROR(H5E_MAP, H5E_CANTSET, FAIL, "Map set failed: %d", ret_value);

done:
    D_FUNC_LEAVE
} /* end H5_daos_map_set() */


herr_t 
H5_daos_map_get(void *_map, hid_t key_mem_type_id, const void *key,
    hid_t val_mem_type_id, void *value, hid_t DV_ATTR_UNUSED dxpl_id,
    void DV_ATTR_UNUSED **req)
{
    H5_daos_map_t *map = (H5_daos_map_t *)_map;
    size_t key_size, val_size;
    hbool_t val_is_vl;
    char const_akey[] = H5_DAOS_MAP_KEY;
    daos_key_t dkey;
    daos_iod_t iod;
    daos_sg_list_t sgl;
    daos_iov_t sg_iov;
    H5T_class_t cls;
    herr_t ret_value = SUCCEED;

    /* get the key size and checksum from the provdied key datatype & buffer */
    if(H5_daos_map_get_size(key_mem_type_id, key, NULL, &key_size, NULL) < 0)
        D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "can't get key size");

    /* get information about the datatype of the value. Get the values
       size if it is not VL. val_size will be 0 if it is VL */
    if(H5_daos_map_dtype_info(val_mem_type_id, &val_is_vl, &val_size, &cls) < 0)
        D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "can't get key size");

        /* Set up dkey */
        daos_iov_set(&dkey, (void *)key, (daos_size_t)key_size);
        /* Set up iod */
        memset(&iod, 0, sizeof(iod));
        daos_iov_set(&iod.iod_name, const_akey, (daos_size_t)(sizeof(const_akey) - 1));
        daos_csum_set(&iod.iod_kcsum, NULL, 0);
        iod.iod_nr = 1u;
        iod.iod_type = DAOS_IOD_SINGLE;

    if (!val_is_vl) {
        iod.iod_size = (daos_size_t)val_size;

        /* Set up sgl */
        daos_iov_set(&sg_iov, value, (daos_size_t)val_size);
        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &sg_iov;

        if(0 != (ret_value = daos_obj_fetch(map->obj.obj_oh, 
                            DAOS_TX_NONE, &dkey,
                            1, &iod, &sgl, NULL , NULL)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "MAP get failed: %d", ret_value);
    }
    else {
        iod.iod_size = DAOS_REC_ANY;
        if(0 != (ret_value = daos_obj_fetch(map->obj.obj_oh, 
                            DAOS_TX_NONE, &dkey,
                            1, &iod, NULL, NULL , NULL)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "MAP get failed: %d", ret_value);

        val_size = iod.iod_size;

        if(cls == H5T_STRING) {
            char *val;

            val = (char *)malloc(val_size);
            daos_iov_set(&sg_iov, val, (daos_size_t)val_size);
            (*(void **) value) = val;
        }
        else {
            hvl_t *vl_buf = (hvl_t *)value;

            assert(H5T_VLEN == cls);

            vl_buf->len = val_size;
            vl_buf->p = malloc(val_size);
            daos_iov_set(&sg_iov, vl_buf->p, (daos_size_t)val_size);
        }

        sgl.sg_nr = 1;
        sgl.sg_nr_out = 0;
        sgl.sg_iovs = &sg_iov;

        if(0 != (ret_value = daos_obj_fetch(map->obj.obj_oh, 
                            DAOS_TX_NONE, &dkey,
                            1, &iod, &sgl, NULL , NULL)))
            D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "MAP get failed: %d", ret_value);
    }

done:
    D_FUNC_LEAVE
} /* end H5_daos_map_get() */


herr_t 
H5_daos_map_get_types(void *_map, hid_t *key_type_id, hid_t *val_type_id,
    void DV_ATTR_UNUSED **req)
{
    H5_daos_map_t *map = (H5_daos_map_t *)_map;
    herr_t ret_value = SUCCEED;

    if((*key_type_id = H5Tcopy(map->ktype_id)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTGET, FAIL, "can't get datatype ID of map key");

    if((*val_type_id = H5Tcopy(map->vtype_id)) < 0)
        D_GOTO_ERROR(H5E_ARGS, H5E_CANTGET, FAIL, "can't get datatype ID of map val");

done:
    D_FUNC_LEAVE
} /* end H5_daos_map_get_types() */


#define ENUM_DESC_BUF   512
#define ENUM_DESC_NR    5

herr_t 
H5_daos_map_get_count(void *_map, hsize_t *count, void DV_ATTR_UNUSED **req)
{
    H5_daos_map_t *map = (H5_daos_map_t *)_map;
    char        *buf;
    daos_key_desc_t  kds[ENUM_DESC_NR];
    daos_anchor_t  anchor;
    uint32_t     number;
    hsize_t      key_nr;
    daos_sg_list_t   sgl;
    daos_iov_t   sg_iov;
    herr_t       ret_value = SUCCEED;

    memset(&anchor, 0, sizeof(anchor));
    buf = (char *)malloc(ENUM_DESC_BUF);

    daos_iov_set(&sg_iov, buf, ENUM_DESC_BUF);
    sgl.sg_nr = 1;
    sgl.sg_nr_out = 0;
    sgl.sg_iovs = &sg_iov;

    for (number = ENUM_DESC_NR, key_nr = 0; !daos_anchor_is_eof(&anchor);
            number = ENUM_DESC_NR) {
        memset(buf, 0, ENUM_DESC_BUF);

        ret_value = daos_obj_list_dkey(map->obj.obj_oh, 
                           DAOS_TX_NONE,
                           &number, kds, &sgl, &anchor, NULL);
        if(ret_value != 0)
            D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "Map List failed: %d", ret_value);
        if (number == 0)
            continue; /* loop should break for EOF */

        key_nr += (hsize_t)number;
    }

    /* -1 for MD dkey */
    *count = (hsize_t)(key_nr - 1);

done:
    D_FUNC_LEAVE
} /* end H5_daos_map_get_count() */


herr_t 
H5_daos_map_exists(void *_map, hid_t key_mem_type_id, const void *key,
    hbool_t *exists, void DV_ATTR_UNUSED **req)
{
    H5_daos_map_t *map = (H5_daos_map_t *)_map;
    size_t key_size;
    char const_akey[] = H5_DAOS_MAP_KEY;
    daos_key_t dkey;
    daos_iod_t iod;
    herr_t ret_value = SUCCEED;

    /* get the key size and checksum from the provdied key datatype & buffer */
    if(H5_daos_map_get_size(key_mem_type_id, key, NULL, &key_size, NULL) < 0)
        D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "can't get key size");

    /* Set up dkey */
    daos_iov_set(&dkey, (void *)key, (daos_size_t)key_size);
    /* Set up iod */
    memset(&iod, 0, sizeof(iod));
    daos_iov_set(&iod.iod_name, (void *)const_akey, (daos_size_t)(sizeof(const_akey) - 1));
    daos_csum_set(&iod.iod_kcsum, NULL, 0);
    iod.iod_nr = 1u;
    iod.iod_type = DAOS_IOD_SINGLE;
    iod.iod_size = DAOS_REC_ANY;

    if(0 != (ret_value = daos_obj_fetch(map->obj.obj_oh, 
                        DAOS_TX_NONE, &dkey,
                        1, &iod, NULL, NULL , NULL)))
        D_GOTO_ERROR(H5E_MAP, H5E_CANTGET, FAIL, "MAP get failed: %d", ret_value);

    if(iod.iod_size != 0)
        *exists = TRUE;
    else
        *exists = FALSE;

done:
    D_FUNC_LEAVE
} /* end H5_daos_map_exists() */


/*-------------------------------------------------------------------------
 * Function:    H5_daos_map_close
 *
 * Purpose:     Closes a DAOS HDF5 map.
 *
 * Return:      Success:        0
 *              Failure:        -1
 *
 * Programmer:  Neil Fortner
 *              November, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5_daos_map_close(void *_map, hid_t DV_ATTR_UNUSED dxpl_id,
    void DV_ATTR_UNUSED **req)
{
    H5_daos_map_t *map = (H5_daos_map_t *)_map;
    int ret;
    herr_t ret_value = SUCCEED;

    assert(map);

    if(--map->obj.item.rc == 0) {
        /* Free map data structures */
        if(!daos_handle_is_inval(map->obj.obj_oh))
            if(0 != (ret = daos_obj_close(map->obj.obj_oh, NULL /*event*/)))
                D_DONE_ERROR(H5E_MAP, H5E_CANTCLOSEOBJ, FAIL, "can't close map DAOS object: %d", ret)
        if(map->ktype_id != FAIL && H5I_dec_app_ref(map->ktype_id) < 0)
            D_DONE_ERROR(H5E_MAP, H5E_CANTDEC, FAIL, "failed to close datatype")
        if(map->vtype_id != FAIL && H5I_dec_app_ref(map->vtype_id) < 0)
            D_DONE_ERROR(H5E_MAP, H5E_CANTDEC, FAIL, "failed to close datatype")
        map = H5FL_FREE(H5_daos_map_t, map);
    } /* end if */

    D_FUNC_LEAVE
} /* end H5_daos_map_close() */
#endif /* DV_HAVE_MAP */

H5PL_type_t
H5PLget_plugin_type(void) {
    return H5PL_TYPE_VOL;
}

const void*
H5PLget_plugin_info(void) {
    return &H5_daos_g;
}
