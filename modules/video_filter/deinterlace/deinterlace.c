/*****************************************************************************
 * deinterlace.c : deinterlacer plugin for vlc
 *****************************************************************************
 * Copyright (C) 2000, 2001 VideoLAN
 * $Id: deinterlace.c,v 1.3 2002/10/11 21:17:29 sam Exp $
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <errno.h>
#include <stdlib.h>                                      /* malloc(), free() */
#include <string.h>

#include <vlc/vlc.h>
#include <vlc/vout.h>

#include "../filter_common.h"

#define DEINTERLACE_DISCARD 1
#define DEINTERLACE_MEAN    2
#define DEINTERLACE_BLEND   3
#define DEINTERLACE_BOB     4
#define DEINTERLACE_LINEAR  5

/*****************************************************************************
 * Local protypes
 *****************************************************************************/
static int  Create    ( vlc_object_t * );
static void Destroy   ( vlc_object_t * );

static int  Init      ( vout_thread_t * );
static void End       ( vout_thread_t * );
static void Render    ( vout_thread_t *, picture_t * );

static void RenderBob    ( vout_thread_t *, picture_t *, picture_t *, int );
static void RenderMean   ( vout_thread_t *, picture_t *, picture_t * );
static void RenderBlend  ( vout_thread_t *, picture_t *, picture_t * );
static void RenderLinear ( vout_thread_t *, picture_t *, picture_t *, int );

static void Merge        ( void *, const void *, const void *, size_t );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define MODE_TEXT N_("deinterlace mode")
#define MODE_LONGTEXT N_("One of \"discard\", \"blend\", \"mean\", \"bob\" or \"linear\"")

static char *mode_list[] = { "discard", "blend", "mean", "bob", "linear", NULL };

vlc_module_begin();
    add_category_hint( N_("Miscellaneous"), NULL );
    add_string_from_list( "deinterlace-mode", "discard", mode_list, NULL,
                          MODE_TEXT, MODE_LONGTEXT );
    set_description( _("deinterlacing module") );
    set_capability( "video filter", 0 );
    add_shortcut( "deinterlace" );
    set_callbacks( Create, Destroy );
vlc_module_end();

/*****************************************************************************
 * vout_sys_t: Deinterlace video output method descriptor
 *****************************************************************************
 * This structure is part of the video output thread descriptor.
 * It describes the Deinterlace specific properties of an output thread.
 *****************************************************************************/
struct vout_sys_t
{
    int        i_mode;        /* Deinterlace mode */
    vlc_bool_t b_double_rate; /* Shall we double the framerate? */

    mtime_t    last_date;
    mtime_t    next_date;

    vout_thread_t *p_vout;
};

/*****************************************************************************
 * Create: allocates Deinterlace video thread output method
 *****************************************************************************
 * This function allocates and initializes a Deinterlace vout method.
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{   
    vout_thread_t *p_vout = (vout_thread_t *)p_this;
    char *psz_method;

    /* Allocate structure */
    p_vout->p_sys = malloc( sizeof( vout_sys_t ) );
    if( p_vout->p_sys == NULL )
    {
        msg_Err( p_vout, "out of memory" );
        return 1;
    }

    p_vout->pf_init = Init;
    p_vout->pf_end = End;
    p_vout->pf_manage = NULL;
    p_vout->pf_render = Render;
    p_vout->pf_display = NULL;

    p_vout->p_sys->i_mode = DEINTERLACE_DISCARD;
    p_vout->p_sys->b_double_rate = 0;
    p_vout->p_sys->last_date = 0;

    /* Look what method was requested */
    psz_method = config_GetPsz( p_vout, "deinterlace-mode" );

    if( psz_method == NULL )
    {
        msg_Err( p_vout, "configuration variable %s empty",
                         "deinterlace-mode" );
        msg_Err( p_vout, "no deinterlace mode provided, using \"discard\"" );
    }
    else
    {
        if( !strcmp( psz_method, "discard" ) )
        {
            p_vout->p_sys->i_mode = DEINTERLACE_DISCARD;
        }
        else if( !strcmp( psz_method, "mean" ) )
        {
            p_vout->p_sys->i_mode = DEINTERLACE_MEAN;
        }
        else if( !strcmp( psz_method, "blend" )
                  || !strcmp( psz_method, "average" )
                  || !strcmp( psz_method, "combine-fields" ) )
        {
            p_vout->p_sys->i_mode = DEINTERLACE_BLEND;
        }
        else if( !strcmp( psz_method, "bob" )
                  || !strcmp( psz_method, "progressive-scan" ) )
        {
            p_vout->p_sys->i_mode = DEINTERLACE_BOB;
            p_vout->p_sys->b_double_rate = 1;
        }
        else if( !strcmp( psz_method, "linear" ) )
        {
            p_vout->p_sys->i_mode = DEINTERLACE_LINEAR;
            p_vout->p_sys->b_double_rate = 1;
        }
        else
        {
            msg_Err( p_vout, "no valid deinterlace mode provided, "
                             "using \"discard\"" );
        }

        free( psz_method );
    }

    return 0;
}

/*****************************************************************************
 * Init: initialize Deinterlace video thread output method
 *****************************************************************************/
static int Init( vout_thread_t *p_vout )
{
    int i_index;
    picture_t *p_pic;
    
    I_OUTPUTPICTURES = 0;

    /* Initialize the output structure, full of directbuffers since we want
     * the decoder to output directly to our structures. */
    switch( p_vout->render.i_chroma )
    {
        case VLC_FOURCC('I','4','2','0'):
        case VLC_FOURCC('I','Y','U','V'):
        case VLC_FOURCC('Y','V','1','2'):
        case VLC_FOURCC('I','4','2','2'):
            p_vout->output.i_chroma = p_vout->render.i_chroma;
            p_vout->output.i_width  = p_vout->render.i_width;
            p_vout->output.i_height = p_vout->render.i_height;
            p_vout->output.i_aspect = p_vout->render.i_aspect;
            break;

        default:
            return 0; /* unknown chroma */
            break;
    }

    /* Try to open the real video output, with half the height our images */
    msg_Dbg( p_vout, "spawning the real video output" );

    switch( p_vout->render.i_chroma )
    {
    case VLC_FOURCC('I','4','2','0'):
    case VLC_FOURCC('I','Y','U','V'):
    case VLC_FOURCC('Y','V','1','2'):
        switch( p_vout->p_sys->i_mode )
        {
        case DEINTERLACE_BOB:
        case DEINTERLACE_MEAN:
        case DEINTERLACE_DISCARD:
            p_vout->p_sys->p_vout =
                vout_CreateThread( p_vout,
                       p_vout->output.i_width, p_vout->output.i_height / 2,
                       p_vout->output.i_chroma, p_vout->output.i_aspect );
            break;

        case DEINTERLACE_BLEND:
        case DEINTERLACE_LINEAR:
            p_vout->p_sys->p_vout =
                vout_CreateThread( p_vout,
                       p_vout->output.i_width, p_vout->output.i_height,
                       p_vout->output.i_chroma, p_vout->output.i_aspect );
            break;
        }
        break;

    case VLC_FOURCC('I','4','2','2'):
        p_vout->p_sys->p_vout =
            vout_CreateThread( p_vout,
                       p_vout->output.i_width, p_vout->output.i_height,
                       VLC_FOURCC('I','4','2','0'), p_vout->output.i_aspect );
        break;

    default:
        break;
    }

    /* Everything failed */
    if( p_vout->p_sys->p_vout == NULL )
    {
        msg_Err( p_vout, "cannot open vout, aborting" );

        return 0;
    }
 
    ALLOCATE_DIRECTBUFFERS( VOUT_MAX_PICTURES );

    return 0;
}

/*****************************************************************************
 * End: terminate Deinterlace video thread output method
 *****************************************************************************/
static void End( vout_thread_t *p_vout )
{
    int i_index;

    /* Free the fake output buffers we allocated */
    for( i_index = I_OUTPUTPICTURES ; i_index ; )
    {
        i_index--;
        free( PP_OUTPUTPICTURE[ i_index ]->p_data_orig );
    }
}

/*****************************************************************************
 * Destroy: destroy Deinterlace video thread output method
 *****************************************************************************
 * Terminate an output method created by DeinterlaceCreateOutputMethod
 *****************************************************************************/
static void Destroy( vlc_object_t *p_this )
{
    vout_thread_t *p_vout = (vout_thread_t *)p_this;

    vout_DestroyThread( p_vout->p_sys->p_vout );

    free( p_vout->p_sys );
}

/*****************************************************************************
 * Render: displays previously rendered output
 *****************************************************************************
 * This function send the currently rendered image to Deinterlace image,
 * waits until it is displayed and switch the two rendering buffers, preparing
 * next frame.
 *****************************************************************************/
static void Render ( vout_thread_t *p_vout, picture_t *p_pic )
{
    picture_t *pp_outpic[2];

    /* Get a new picture */
    while( ( pp_outpic[0] = vout_CreatePicture( p_vout->p_sys->p_vout,
                                             0, 0, 0 ) )
              == NULL )
    {
        if( p_vout->b_die || p_vout->b_error )
        {
            return;
        }
        msleep( VOUT_OUTMEM_SLEEP );
    }

    vout_DatePicture( p_vout->p_sys->p_vout, pp_outpic[0], p_pic->date );

    /* If we are using double rate, get an additional new picture */
    if( p_vout->p_sys->b_double_rate )
    {
        while( ( pp_outpic[1] = vout_CreatePicture( p_vout->p_sys->p_vout,
                                                 0, 0, 0 ) )
                  == NULL )
        {
            if( p_vout->b_die || p_vout->b_error )
            {
                vout_DestroyPicture( p_vout->p_sys->p_vout, pp_outpic[0] );
                return;
            }
            msleep( VOUT_OUTMEM_SLEEP );
        }   

        /* 20ms is a bit arbitrary, but it's only for the first image we get */
        if( !p_vout->p_sys->last_date )
        {
            vout_DatePicture( p_vout->p_sys->p_vout, pp_outpic[1],
                              p_pic->date + 20000 );
        }
        else
        {
            vout_DatePicture( p_vout->p_sys->p_vout, pp_outpic[1],
                      (3 * p_pic->date - p_vout->p_sys->last_date) / 2 );
        }
        p_vout->p_sys->last_date = p_pic->date;
    }

    switch( p_vout->p_sys->i_mode )
    {
        case DEINTERLACE_DISCARD:
            RenderBob( p_vout, pp_outpic[0], p_pic, 0 );
            vout_DisplayPicture( p_vout->p_sys->p_vout, pp_outpic[0] );
            break;

        case DEINTERLACE_BOB:
            RenderBob( p_vout, pp_outpic[0], p_pic, 0 );
            vout_DisplayPicture( p_vout->p_sys->p_vout, pp_outpic[0] );
            RenderBob( p_vout, pp_outpic[1], p_pic, 1 );
            vout_DisplayPicture( p_vout->p_sys->p_vout, pp_outpic[1] );
            break;

        case DEINTERLACE_LINEAR:
            RenderLinear( p_vout, pp_outpic[0], p_pic, 0 );
            vout_DisplayPicture( p_vout->p_sys->p_vout, pp_outpic[0] );
            RenderLinear( p_vout, pp_outpic[1], p_pic, 1 );
            vout_DisplayPicture( p_vout->p_sys->p_vout, pp_outpic[1] );
            break;

        case DEINTERLACE_MEAN:
            RenderMean( p_vout, pp_outpic[0], p_pic );
            vout_DisplayPicture( p_vout->p_sys->p_vout, pp_outpic[0] );
            break;

        case DEINTERLACE_BLEND:
            RenderBlend( p_vout, pp_outpic[0], p_pic );
            vout_DisplayPicture( p_vout->p_sys->p_vout, pp_outpic[0] );
            break;
    }
}

/*****************************************************************************
 * RenderBob: renders a bob picture
 *****************************************************************************/
static void RenderBob( vout_thread_t *p_vout,
                       picture_t *p_outpic, picture_t *p_pic, int i_field )
{
    int i_plane;

    /* Copy image and skip lines */
    for( i_plane = 0 ; i_plane < p_pic->i_planes ; i_plane++ )
    {
        u8 *p_in, *p_out_end, *p_out;
        int i_increment;

        p_in = p_pic->p[i_plane].p_pixels
                   + i_field * p_pic->p[i_plane].i_pitch;

        p_out = p_outpic->p[i_plane].p_pixels;
        p_out_end = p_out + p_outpic->p[i_plane].i_pitch
                             * p_outpic->p[i_plane].i_lines;

        switch( p_vout->render.i_chroma )
        {
        case VLC_FOURCC('I','4','2','0'):
        case VLC_FOURCC('I','Y','U','V'):
        case VLC_FOURCC('Y','V','1','2'):

            for( ; p_out < p_out_end ; )
            {
                p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                          p_pic->p[i_plane].i_pitch );

                p_out += p_pic->p[i_plane].i_pitch;
                p_in += 2 * p_pic->p[i_plane].i_pitch;
            }
            break;

        case VLC_FOURCC('I','4','2','2'):

            i_increment = 2 * p_pic->p[i_plane].i_pitch;

            if( i_plane == Y_PLANE )
            {
                for( ; p_out < p_out_end ; )
                {
                    p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                              p_pic->p[i_plane].i_pitch );
                    p_out += p_pic->p[i_plane].i_pitch;
                    p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                              p_pic->p[i_plane].i_pitch );
                    p_out += p_pic->p[i_plane].i_pitch;
                    p_in += i_increment;
                }
            }
            else
            {
                for( ; p_out < p_out_end ; )
                {
                    p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                              p_pic->p[i_plane].i_pitch );
                    p_out += p_pic->p[i_plane].i_pitch;
                    p_in += i_increment;
                }
            }
            break;

        default:
            break;
        }
    }
}

/*****************************************************************************
 * RenderLinear: displays previously rendered output
 *****************************************************************************/
static void RenderLinear( vout_thread_t *p_vout,
                          picture_t *p_outpic, picture_t *p_pic, int i_field )
{
    int i_plane;

    /* Copy image and skip lines */
    for( i_plane = 0 ; i_plane < p_pic->i_planes ; i_plane++ )
    {
        u8 *p_in, *p_out_end, *p_out;

        p_in = p_pic->p[i_plane].p_pixels;
        p_out = p_outpic->p[i_plane].p_pixels;
        p_out_end = p_out + p_outpic->p[i_plane].i_pitch
                             * p_outpic->p[i_plane].i_lines;

        /* For BOTTOM field we need to add the first line */
        if( i_field == 1 )
        {
            p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                      p_pic->p[i_plane].i_pitch );
            p_in += p_pic->p[i_plane].i_pitch;
            p_out += p_pic->p[i_plane].i_pitch;
        }

        p_out_end -= 2 * p_outpic->p[i_plane].i_pitch;

        for( ; p_out < p_out_end ; )
        {
            p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                      p_pic->p[i_plane].i_pitch );

            p_out += p_pic->p[i_plane].i_pitch;

            Merge( p_out, p_in, p_in + 2 * p_pic->p[i_plane].i_pitch,
                   p_pic->p[i_plane].i_pitch );

            p_in += 2 * p_pic->p[i_plane].i_pitch;
            p_out += p_pic->p[i_plane].i_pitch;
        }

        p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                  p_pic->p[i_plane].i_pitch );

        /* For TOP field we need to add the last line */
        if( i_field == 0 )
        {
            p_in += p_pic->p[i_plane].i_pitch;
            p_out += p_pic->p[i_plane].i_pitch;
            p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                      p_pic->p[i_plane].i_pitch );
        }
    }
}

static void RenderMean( vout_thread_t *p_vout,
                        picture_t *p_outpic, picture_t *p_pic )
{
    int i_plane;

    /* Copy image and skip lines */
    for( i_plane = 0 ; i_plane < p_pic->i_planes ; i_plane++ )
    {
        u8 *p_in, *p_out_end, *p_out;

        p_in = p_pic->p[i_plane].p_pixels;

        p_out = p_outpic->p[i_plane].p_pixels;
        p_out_end = p_out + p_outpic->p[i_plane].i_pitch
                             * p_outpic->p[i_plane].i_lines;

        /* All lines: mean value */
        for( ; p_out < p_out_end ; )
        {
            Merge( p_out, p_in, p_in + p_pic->p[i_plane].i_pitch,
                   p_pic->p[i_plane].i_pitch );

            p_out += p_pic->p[i_plane].i_pitch;
            p_in += 2 * p_pic->p[i_plane].i_pitch;
        }
    }
}

static void RenderBlend( vout_thread_t *p_vout,
                         picture_t *p_outpic, picture_t *p_pic )
{
    int i_plane;

    /* Copy image and skip lines */
    for( i_plane = 0 ; i_plane < p_pic->i_planes ; i_plane++ )
    {
        u8 *p_in, *p_out_end, *p_out;

        p_in = p_pic->p[i_plane].p_pixels;

        p_out = p_outpic->p[i_plane].p_pixels;
        p_out_end = p_out + p_outpic->p[i_plane].i_pitch
                             * p_outpic->p[i_plane].i_lines;

        /* First line: simple copy */
        p_vout->p_vlc->pf_memcpy( p_out, p_in,
                                  p_pic->p[i_plane].i_pitch );
        p_out += p_pic->p[i_plane].i_pitch;

        /* Remaining lines: mean value */
        for( ; p_out < p_out_end ; )
        {
            Merge( p_out, p_in, p_in + p_pic->p[i_plane].i_pitch,
                   p_pic->p[i_plane].i_pitch );

            p_out += p_pic->p[i_plane].i_pitch;
            p_in += p_pic->p[i_plane].i_pitch;
        }
    }
}

static void Merge( void *p_dest, const void *p_s1,
                   const void *p_s2, size_t i_bytes )
{
    u8* p_end = (u8*)p_dest + i_bytes - 8;

    while( (u8*)p_dest < p_end )
    {
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
    }

    p_end += 8;

    while( (u8*)p_dest < p_end )
    {
        *(u8*)p_dest++ = ( (u16)(*(u8*)p_s1++) + (u16)(*(u8*)p_s2++) ) >> 1;
    }
}
