/*
 * Copyright © 2011,2014  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Google Author(s): Behdad Esfahbod, Roozbeh Pournader
 */

#include "hb-private.hh"

#include "hb-ot.h"

#include "hb-font-private.hh"

#include "hb-ot-cmap-table.hh"
#include "hb-ot-glyf-table.hh"
#include "hb-ot-head-table.hh"
#include "hb-ot-hhea-table.hh"
#include "hb-ot-hmtx-table.hh"
#include "hb-ot-os2-table.hh"
//#include "hb-ot-post-table.hh"


struct hb_ot_face_metrics_accelerator_t
{
  unsigned int num_metrics;
  unsigned int num_advances;
  unsigned int default_advance;
  unsigned short ascender;
  unsigned short descender;
  unsigned short line_gap;

  const OT::_mtx *table;
  hb_blob_t *blob;

  inline void init (hb_face_t *face,
		    hb_tag_t _hea_tag,
		    hb_tag_t _mtx_tag,
		    hb_tag_t os2_tag)
  {
    this->default_advance = face->get_upem ();

    bool got_font_extents = false;
    if (os2_tag)
    {
      hb_blob_t *os2_blob = OT::Sanitizer<OT::os2>::sanitize (face->reference_table (os2_tag));
      const OT::os2 *os2 = OT::Sanitizer<OT::os2>::lock_instance (os2_blob);
#define USE_TYPO_METRICS (1u<<7)
      if (0 != (os2->fsSelection & USE_TYPO_METRICS))
      {
	this->ascender = os2->sTypoAscender;
	this->descender = os2->sTypoDescender;
	this->line_gap = os2->sTypoLineGap;
	got_font_extents = (this->ascender | this->descender) != 0;
      }
      hb_blob_destroy (os2_blob);
    }

    hb_blob_t *_hea_blob = OT::Sanitizer<OT::_hea>::sanitize (face->reference_table (_hea_tag));
    const OT::_hea *_hea = OT::Sanitizer<OT::_hea>::lock_instance (_hea_blob);
    this->num_advances = _hea->numberOfLongMetrics;
    if (!got_font_extents)
    {
      this->ascender = _hea->ascender;
      this->descender = _hea->descender;
      this->line_gap = _hea->lineGap;
    }
    hb_blob_destroy (_hea_blob);

    this->blob = OT::Sanitizer<OT::_mtx>::sanitize (face->reference_table (_mtx_tag));

    /* Cap num_metrics() and num_advances() based on table length. */
    unsigned int len = hb_blob_get_length (this->blob);
    if (unlikely (this->num_advances * 4 > len))
      this->num_advances = len / 4;
    this->num_metrics = this->num_advances + (len - 4 * this->num_advances) / 2;

    /* We MUST set num_metrics to zero if num_advances is zero.
     * Our get_advance() depends on that. */
    if (unlikely (!this->num_advances))
    {
      this->num_metrics = this->num_advances = 0;
      hb_blob_destroy (this->blob);
      this->blob = hb_blob_get_empty ();
    }
    this->table = OT::Sanitizer<OT::_mtx>::lock_instance (this->blob);
  }

  inline void fini (void)
  {
    hb_blob_destroy (this->blob);
  }

  inline unsigned int get_advance (hb_codepoint_t glyph) const
  {
    if (unlikely (glyph >= this->num_metrics))
    {
      /* If this->num_metrics is zero, it means we don't have the metrics table
       * for this direction: return default advance.  Otherwise, it means that the
       * glyph index is out of bound: return zero. */
      if (this->num_metrics)
	return 0;
      else
	return this->default_advance;
    }

    if (glyph >= this->num_advances)
      glyph = this->num_advances - 1;

    return this->table->longMetric[glyph].advance;
  }
};

struct hb_ot_face_glyf_accelerator_t
{
  bool short_offset;
  unsigned int num_glyphs;
  const OT::loca *loca;
  const OT::glyf *glyf;
  hb_blob_t *loca_blob;
  hb_blob_t *glyf_blob;
  unsigned int glyf_len;

  inline void init (hb_face_t *face)
  {
    hb_blob_t *head_blob = OT::Sanitizer<OT::head>::sanitize (face->reference_table (HB_OT_TAG_head));
    const OT::head *head = OT::Sanitizer<OT::head>::lock_instance (head_blob);
    if ((unsigned int) head->indexToLocFormat > 1 || head->glyphDataFormat != 0)
    {
      /* Unknown format.  Leave num_glyphs=0, that takes care of disabling us. */
      hb_blob_destroy (head_blob);
      return;
    }
    this->short_offset = 0 == head->indexToLocFormat;
    hb_blob_destroy (head_blob);

    this->loca_blob = OT::Sanitizer<OT::loca>::sanitize (face->reference_table (HB_OT_TAG_loca));
    this->loca = OT::Sanitizer<OT::loca>::lock_instance (this->loca_blob);
    this->glyf_blob = OT::Sanitizer<OT::glyf>::sanitize (face->reference_table (HB_OT_TAG_glyf));
    this->glyf = OT::Sanitizer<OT::glyf>::lock_instance (this->glyf_blob);

    this->num_glyphs = MAX (1u, hb_blob_get_length (this->loca_blob) / (this->short_offset ? 2 : 4)) - 1;
    this->glyf_len = hb_blob_get_length (this->glyf_blob);
  }

  inline void fini (void)
  {
    hb_blob_destroy (this->loca_blob);
    hb_blob_destroy (this->glyf_blob);
  }

  inline bool get_extents (hb_codepoint_t glyph,
			   hb_glyph_extents_t *extents) const
  {
    if (unlikely (glyph >= this->num_glyphs))
      return false;

    unsigned int start_offset, end_offset;
    if (this->short_offset)
    {
      start_offset = 2 * this->loca->u.shortsZ[glyph];
      end_offset   = 2 * this->loca->u.shortsZ[glyph + 1];
    }
    else
    {
      start_offset = this->loca->u.longsZ[glyph];
      end_offset   = this->loca->u.longsZ[glyph + 1];
    }

    if (start_offset > end_offset || end_offset > this->glyf_len)
      return false;

    if (end_offset - start_offset < OT::glyfGlyphHeader::static_size)
      return true; /* Empty glyph; zero extents. */

    const OT::glyfGlyphHeader &glyph_header = OT::StructAtOffset<OT::glyfGlyphHeader> (this->glyf, start_offset);

    extents->x_bearing = MIN (glyph_header.xMin, glyph_header.xMax);
    extents->y_bearing = MAX (glyph_header.yMin, glyph_header.yMax);
    extents->width     = MAX (glyph_header.xMin, glyph_header.xMax) - extents->x_bearing;
    extents->height    = MIN (glyph_header.yMin, glyph_header.yMax) - extents->y_bearing;

    return true;
  }
};

typedef bool (*hb_cmap_get_glyph_func_t) (const void *obj,
					  hb_codepoint_t codepoint,
					  hb_codepoint_t *glyph);

template <typename Type>
static inline bool get_glyph_from (const void *obj,
				   hb_codepoint_t codepoint,
				   hb_codepoint_t *glyph)
{
  const Type *typed_obj = (const Type *) obj;
  return typed_obj->get_glyph (codepoint, glyph);
}

struct hb_ot_face_cmap_accelerator_t
{
  hb_cmap_get_glyph_func_t get_glyph_func;
  const void *get_glyph_data;
  OT::CmapSubtableFormat4::accelerator_t format4_accel;

  const OT::CmapSubtableFormat14 *uvs_table;
  hb_blob_t *blob;

  inline void init (hb_face_t *face)
  {
    this->blob = OT::Sanitizer<OT::cmap>::sanitize (face->reference_table (HB_OT_TAG_cmap));
    const OT::cmap *cmap = OT::Sanitizer<OT::cmap>::lock_instance (this->blob);
    const OT::CmapSubtable *subtable = NULL;
    const OT::CmapSubtableFormat14 *subtable_uvs = NULL;

    /* 32-bit subtables. */
    if (!subtable) subtable = cmap->find_subtable (3, 10);
    if (!subtable) subtable = cmap->find_subtable (0, 6);
    if (!subtable) subtable = cmap->find_subtable (0, 4);
    /* 16-bit subtables. */
    if (!subtable) subtable = cmap->find_subtable (3, 1);
    if (!subtable) subtable = cmap->find_subtable (0, 3);
    if (!subtable) subtable = cmap->find_subtable (0, 2);
    if (!subtable) subtable = cmap->find_subtable (0, 1);
    if (!subtable) subtable = cmap->find_subtable (0, 0);
    if (!subtable) subtable = cmap->find_subtable (3, 0);
    /* Meh. */
    if (!subtable) subtable = &OT::Null(OT::CmapSubtable);

    /* UVS subtable. */
    if (!subtable_uvs)
    {
      const OT::CmapSubtable *st = cmap->find_subtable (0, 5);
      if (st && st->u.format == 14)
        subtable_uvs = &st->u.format14;
    }
    /* Meh. */
    if (!subtable_uvs) subtable_uvs = &OT::Null(OT::CmapSubtableFormat14);

    this->uvs_table = subtable_uvs;

    this->get_glyph_data = subtable;
    switch (subtable->u.format) {
    /* Accelerate format 4 and format 12. */
    default: this->get_glyph_func = get_glyph_from<OT::CmapSubtable>;		break;
    case 12: this->get_glyph_func = get_glyph_from<OT::CmapSubtableFormat12>;	break;
    case  4:
      {
        this->format4_accel.init (&subtable->u.format4);
	this->get_glyph_data = &this->format4_accel;
        this->get_glyph_func = this->format4_accel.get_glyph_func;
      }
      break;
    }
  }

  inline void fini (void)
  {
    hb_blob_destroy (this->blob);
  }

  inline bool get_nominal_glyph (hb_codepoint_t  unicode,
				 hb_codepoint_t *glyph) const
  {
    return this->get_glyph_func (this->get_glyph_data, unicode, glyph);
  }

  inline bool get_variation_glyph (hb_codepoint_t  unicode,
				   hb_codepoint_t  variation_selector,
				   hb_codepoint_t *glyph) const
  {
    switch (this->uvs_table->get_glyph_variant (unicode,
						variation_selector,
						glyph))
    {
      case OT::GLYPH_VARIANT_NOT_FOUND:		return false;
      case OT::GLYPH_VARIANT_FOUND:		return true;
      case OT::GLYPH_VARIANT_USE_DEFAULT:	break;
    }

    return get_nominal_glyph (unicode, glyph);
  }
};

template <typename T>
struct hb_lazy_loader_t
{
  inline void init (hb_face_t *face_)
  {
    face = face_;
    instance = NULL;
  }

  inline void fini (void)
  {
    if (instance && instance != &OT::Null(T))
    {
      instance->fini();
      free (instance);
    }
  }

  inline const T* operator-> (void) const
  {
  retry:
    T *p = (T *) hb_atomic_ptr_get (&instance);
    if (unlikely (!p))
    {
      p = (T *) calloc (1, sizeof (T));
      if (unlikely (!p))
        return &OT::Null(T);
      p->init (face);
      if (unlikely (!hb_atomic_ptr_cmpexch (const_cast<T **>(&instance), NULL, p)))
      {
	p->fini ();
	goto retry;
      }
    }
    return p;
  }

  private:
  hb_face_t *face;
  T *instance;
};

struct hb_ot_font_t
{
  hb_ot_face_cmap_accelerator_t cmap;
  hb_ot_face_metrics_accelerator_t h_metrics;
  hb_ot_face_metrics_accelerator_t v_metrics;
  hb_lazy_loader_t<hb_ot_face_glyf_accelerator_t> glyf;
};


static hb_ot_font_t *
_hb_ot_font_create (hb_face_t *face)
{
  hb_ot_font_t *ot_font = (hb_ot_font_t *) calloc (1, sizeof (hb_ot_font_t));

  if (unlikely (!ot_font))
    return NULL;

  ot_font->cmap.init (face);
  ot_font->h_metrics.init (face, HB_OT_TAG_hhea, HB_OT_TAG_hmtx, HB_OT_TAG_os2);
  ot_font->v_metrics.init (face, HB_OT_TAG_vhea, HB_OT_TAG_vmtx, HB_TAG_NONE); /* TODO Can we do this lazily? */
  ot_font->glyf.init (face);

  return ot_font;
}

static void
_hb_ot_font_destroy (hb_ot_font_t *ot_font)
{
  ot_font->cmap.fini ();
  ot_font->h_metrics.fini ();
  ot_font->v_metrics.fini ();
  ot_font->glyf.fini ();

  free (ot_font);
}


static hb_bool_t
hb_ot_get_nominal_glyph (hb_font_t *font HB_UNUSED,
			 void *font_data,
			 hb_codepoint_t unicode,
			 hb_codepoint_t *glyph,
			 void *user_data HB_UNUSED)

{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  return ot_font->cmap.get_nominal_glyph (unicode, glyph);
}

static hb_bool_t
hb_ot_get_variation_glyph (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t unicode,
			   hb_codepoint_t variation_selector,
			   hb_codepoint_t *glyph,
			   void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  return ot_font->cmap.get_variation_glyph (unicode, variation_selector, glyph);
}

static hb_position_t
hb_ot_get_glyph_h_advance (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t glyph,
			   void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  return font->em_scale_x (ot_font->h_metrics.get_advance (glyph));
}

static hb_position_t
hb_ot_get_glyph_v_advance (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t glyph,
			   void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  return font->em_scale_y (-(int) ot_font->v_metrics.get_advance (glyph));
}

static hb_bool_t
hb_ot_get_glyph_extents (hb_font_t *font HB_UNUSED,
			 void *font_data,
			 hb_codepoint_t glyph,
			 hb_glyph_extents_t *extents,
			 void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  bool ret = ot_font->glyf->get_extents (glyph, extents);
  extents->x_bearing = font->em_scale_x (extents->x_bearing);
  extents->y_bearing = font->em_scale_y (extents->y_bearing);
  extents->width     = font->em_scale_x (extents->width);
  extents->height    = font->em_scale_y (extents->height);
  return ret;
}

static hb_bool_t
hb_ot_get_font_h_extents (hb_font_t *font HB_UNUSED,
			  void *font_data,
			  hb_font_extents_t *metrics,
			  void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  metrics->ascender = font->em_scale_y (ot_font->h_metrics.ascender);
  metrics->descender = font->em_scale_y (ot_font->h_metrics.descender);
  metrics->line_gap = font->em_scale_y (ot_font->h_metrics.line_gap);
  return true;
}

static hb_bool_t
hb_ot_get_font_v_extents (hb_font_t *font HB_UNUSED,
			  void *font_data,
			  hb_font_extents_t *metrics,
			  void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  metrics->ascender = font->em_scale_x (ot_font->v_metrics.ascender);
  metrics->descender = font->em_scale_x (ot_font->v_metrics.descender);
  metrics->line_gap = font->em_scale_x (ot_font->v_metrics.line_gap);
  return true;
}

static hb_font_funcs_t *static_ot_funcs = NULL;

#ifdef HB_USE_ATEXIT
static
void free_static_ot_funcs (void)
{
  hb_font_funcs_destroy (static_ot_funcs);
}
#endif

static hb_font_funcs_t *
_hb_ot_get_font_funcs (void)
{
retry:
  hb_font_funcs_t *funcs = (hb_font_funcs_t *) hb_atomic_ptr_get (&static_ot_funcs);

  if (unlikely (!funcs))
  {
    funcs = hb_font_funcs_create ();

    hb_font_funcs_set_font_h_extents_func (funcs, hb_ot_get_font_h_extents, NULL, NULL);
    hb_font_funcs_set_font_v_extents_func (funcs, hb_ot_get_font_v_extents, NULL, NULL);
    hb_font_funcs_set_nominal_glyph_func (funcs, hb_ot_get_nominal_glyph, NULL, NULL);
    hb_font_funcs_set_variation_glyph_func (funcs, hb_ot_get_variation_glyph, NULL, NULL);
    hb_font_funcs_set_glyph_h_advance_func (funcs, hb_ot_get_glyph_h_advance, NULL, NULL);
    hb_font_funcs_set_glyph_v_advance_func (funcs, hb_ot_get_glyph_v_advance, NULL, NULL);
    //hb_font_funcs_set_glyph_h_origin_func (funcs, hb_ot_get_glyph_h_origin, NULL, NULL);
    //hb_font_funcs_set_glyph_v_origin_func (funcs, hb_ot_get_glyph_v_origin, NULL, NULL);
    //hb_font_funcs_set_glyph_h_kerning_func (funcs, hb_ot_get_glyph_h_kerning, NULL, NULL); TODO
    //hb_font_funcs_set_glyph_v_kerning_func (funcs, hb_ot_get_glyph_v_kerning, NULL, NULL);
    hb_font_funcs_set_glyph_extents_func (funcs, hb_ot_get_glyph_extents, NULL, NULL);
    //hb_font_funcs_set_glyph_contour_point_func (funcs, hb_ot_get_glyph_contour_point, NULL, NULL); TODO
    //hb_font_funcs_set_glyph_name_func (funcs, hb_ot_get_glyph_name, NULL, NULL); TODO
    //hb_font_funcs_set_glyph_from_name_func (funcs, hb_ot_get_glyph_from_name, NULL, NULL); TODO

    hb_font_funcs_make_immutable (funcs);

    if (!hb_atomic_ptr_cmpexch (&static_ot_funcs, NULL, funcs)) {
      hb_font_funcs_destroy (funcs);
      goto retry;
    }

#ifdef HB_USE_ATEXIT
    atexit (free_static_ot_funcs); /* First person registers atexit() callback. */
#endif
  };

  return funcs;
}


/**
 * hb_ot_font_set_funcs:
 *
 * Since: 0.9.28
 **/
void
hb_ot_font_set_funcs (hb_font_t *font)
{
  hb_ot_font_t *ot_font = _hb_ot_font_create (font->face);
  if (unlikely (!ot_font))
    return;

  hb_font_set_funcs (font,
		     _hb_ot_get_font_funcs (),
		     ot_font,
		     (hb_destroy_func_t) _hb_ot_font_destroy);
}
