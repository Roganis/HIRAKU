// license:BSD-3-Clause
// copyright-holders:smf
/*
 * PlayStation GPU emulator
 *
 * Copyright 2003-2014 smf
 *
 */

#include "emu.h"
#include "psx.h"
#include "core/prof.h" // HIRAKU: the frame's time by part

#include "cpu/psx/psx.h"

#include "screen.h"


#define STOP_ON_ERROR ( 0 )

#define LOG_WRITE        (1U << 1)
#define LOG_READ         (1U << 2)
#define LOG_TRANSPARENCY (1U << 3)
#define VERBOSE          (0)
#include "logmacro.h"

// device type definition
DEFINE_DEVICE_TYPE(CXD8514Q,  cxd8514q_device,  "cxd8514q",  "CXD8514Q GPU") // VRAM
DEFINE_DEVICE_TYPE(CXD8538Q,  cxd8538q_device,  "cxd8538q",  "CXD8538Q GPU") // VRAM
DEFINE_DEVICE_TYPE(CXD8561Q,  cxd8561q_device,  "cxd8561q",  "CXD8561Q GPU") // SGRAM
DEFINE_DEVICE_TYPE(CXD8561BQ, cxd8561bq_device, "cxd8561bq", "CXD8561BQ GPU") // SGRAM
DEFINE_DEVICE_TYPE(CXD8561CQ, cxd8561cq_device, "cxd8561cq", "CXD8561CQ GPU") // SGRAM
DEFINE_DEVICE_TYPE(CXD8654Q,  cxd8654q_device,  "cxd8654q",  "CXD8654Q GPU") // SGRAM

psxgpu_device::psxgpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, type, tag, owner, clock)
	, device_video_interface(mconfig, *this)
	, device_palette_interface(mconfig, *this)
	, m_ram(*this, finder_base::DUMMY_TAG)
	, m_vblank_handler(*this)
	, m_vclk{ 0, 0 }
{
}

void psxgpu_device::set_cpu(psxcpu_device* cpu)
{
	cpu->gpu_read().set(*this, FUNC(psxgpu_device::read));
	cpu->gpu_write().set(*this, FUNC(psxgpu_device::write));
	cpu->subdevice<psxdma_device>("dma")->install_read_handler(2, psxdma_device::read_delegate(&psxgpu_device::dma_read, this));
	cpu->subdevice<psxdma_device>("dma")->install_write_handler(2, psxdma_device::write_delegate(&psxgpu_device::dma_write, this));
	vblank_callback().set(*cpu->subdevice<psxirq_device>("irq"), FUNC(psxirq_device::intin0));
}

void psxgpu_device::device_start()
{
	screen().register_vblank_callback(vblank_state_delegate(&psxgpu_device::vblank, this));

	for( int n_colour = 0; n_colour < 0x10000; n_colour++ )
	{
		set_pen_color( n_colour, pal555(n_colour,0, 5, 10) );
	}

	if (type() == CXD8538Q)
	{
		psx_gpu_init( 1 );
	}
	else
	{
		psx_gpu_init( 2 );
	}

	// HIRAKU: the drawing thread
	std::copy_n( p_p_vram, 1024, m_hk_raster.p_p_vram );
	m_hk_rows = ( m_ram->size() / 1024 ) / sizeof( uint16_t );
	m_hk_ring = std::make_unique<uint32_t[]>( HK_RING );
	const char *thread = getenv( "HK_GPU_THREAD" );
	if( thread == nullptr || strcmp( thread, "0" ) != 0 )
		m_hk_thread = std::thread( &psxgpu_device::hk_worker, this );
}

void psxgpu_device::device_reset()
{
	gpu_reset();
}

cxd8514q_device::cxd8514q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8514Q, tag, owner, clock)
{
}

cxd8538q_device::cxd8538q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8538Q, tag, owner, clock)
{
}

cxd8561q_device::cxd8561q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8561Q, tag, owner, clock)
{
}

cxd8561bq_device::cxd8561bq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8561BQ, tag, owner, clock)
{
}

cxd8561cq_device::cxd8561cq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8561CQ, tag, owner, clock)
{
}

cxd8654q_device::cxd8654q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: psxgpu_device(mconfig, CXD8654Q, tag, owner, clock)
{
}

static const int m_p_n_nextpointlist4[] = { 1, 3, 0, 2 };
static const int m_p_n_prevpointlist4[] = { 2, 0, 3, 1 };
static const int m_p_n_nextpointlist4b[] = { 0, 3, 1, 2 };
static const int m_p_n_prevpointlist4b[] = { 0, 2, 3, 1 };
static const int m_p_n_nextpointlist3[] = { 1, 2, 0 };
static const int m_p_n_prevpointlist3[] = { 2, 0, 1 };

#define COORD_X( a ) ( a.sw.l )
#define COORD_Y( a ) ( a.sw.h )
#define S11_COORD_X( a ) util::sext( a.sw.l, 11 )
#define S11_COORD_Y( a ) util::sext( a.sw.h, 11 )
#define SIZE_W( a ) ( a.w.l )
#define SIZE_H( a ) ( a.w.h )
#define BGR_C( a ) ( a.b.h3 )
#define BGR_B( a ) ( a.b.h2 )
#define BGR_G( a ) ( a.b.h )
#define BGR_R( a ) ( a.b.l )
#define TEXTURE_V( a ) ( a.b.h )
#define TEXTURE_U( a ) ( a.b.l )

#if PSXGPU_DEBUG_VIEWER

void psxgpu_device::DebugMeshInit()
{
	int width = screen().width();
	int height = screen().height();

	m_debug.b_mesh = 0;
	m_debug.b_texture = 0;
	m_debug.n_interleave = -1;
	m_debug.b_clear = 1;
	m_debug.n_coord = 0;
	m_debug.n_skip = 0;
	m_debug.mesh = std::make_unique<bitmap_ind16>(width, height );
}

void psxgpu_device::DebugMesh( int n_coordx, int n_coordy )
{
	int width = screen().width();
	int height = screen().height();

	n_coordx += m_n_displaystartx;
	n_coordy += n_displaystarty;

	if( m_debug.b_clear )
	{
		m_debug.mesh->fill(0x0000);
		m_debug.b_clear = 0;
	}

	int n_coord;
	int n_colour = 0x1f;

	for( n_coord = 0; n_coord < m_debug.n_coord; n_coord++ )
	{
		if( n_coordx != m_debug.n_coordx[ n_coord ] ||
			n_coordy != m_debug.n_coordy[ n_coord ] )
		{
			break;
		}
	}

	if( n_coord == m_debug.n_coord && m_debug.n_coord > 1 )
	{
		n_colour = 0xffff;
	}

	for( n_coord = 0; n_coord < m_debug.n_coord; n_coord++ )
	{
		int32_t n_xstart = m_debug.n_coordx[ n_coord ];
		int32_t n_xend = n_coordx;
		int32_t n_xlen;
		if( n_xend > n_xstart )
		{
			n_xlen = n_xend - n_xstart;
		}
		else
		{
			n_xlen = n_xstart - n_xend;
		}

		int32_t n_ystart = m_debug.n_coordy[ n_coord ];
		int32_t n_yend = n_coordy;
		int32_t n_ylen;
		if( n_yend > n_ystart )
		{
			n_ylen = n_yend - n_ystart;
		}
		else
		{
			n_ylen = n_ystart - n_yend;
		}

		int32_t n_len;
		if( n_xlen > n_ylen )
		{
			n_len = n_xlen;
		}
		else
		{
			n_len = n_ylen;
		}

		PAIR n_x; n_x.sw.h = n_xstart; n_x.sw.l = 0;
		PAIR n_y; n_y.sw.h = n_ystart; n_y.sw.l = 0;

		if( n_len == 0 )
		{
			n_len = 1;
		}

		int32_t n_dx = (int32_t)( ( n_xend << 16 ) - n_x.d ) / n_len;
		int32_t n_dy = (int32_t)( ( n_yend << 16 ) - n_y.d ) / n_len;

		while( n_len > 0 )
		{
			if( (int16_t)n_x.w.h >= 0 &&
				(int16_t)n_y.w.h >= 0 &&
				(int16_t)n_x.w.h <= width - 1 &&
				(int16_t)n_y.w.h <= height - 1 )
			{
				if( m_debug.mesh->pix( n_y.w.h, n_x.w.h ) != 0xffff )
					m_debug.mesh->pix( n_y.w.h, n_x.w.h ) = n_colour;
			}

			n_x.d += n_dx;
			n_y.d += n_dy;
			n_len--;
		}
	}

	if( m_debug.n_coord < DEBUG_COORDS )
	{
		m_debug.n_coordx[ m_debug.n_coord ] = n_coordx;
		m_debug.n_coordy[ m_debug.n_coord ] = n_coordy;
		m_debug.n_coord++;
	}
}

void psxgpu_device::DebugMeshEnd()
{
	m_debug.n_coord = 0;
}

void psxgpu_device::DebugCheckKeys()
{
	if( machine().input().code_pressed_once( KEYCODE_M ) )
	{
		m_debug.b_mesh = !m_debug.b_mesh;
		updatevisiblearea();
	}

	if( machine().input().code_pressed_once( KEYCODE_V ) )
	{
		m_debug.b_texture = !m_debug.b_texture;
		updatevisiblearea();
	}

	if( machine().input().code_pressed_once( KEYCODE_I ) )
	{
		if( m_debug.b_texture )
		{
			m_debug.n_interleave++;

			if( m_debug.n_interleave == 2 )
				m_debug.n_interleave = -1;

			if( m_debug.n_interleave == -1 )
				popmessage( "interleave off" );
			else if( m_debug.n_interleave == 0 )
				popmessage( "4 bit interleave" );
			else if( m_debug.n_interleave == 1 )
				popmessage( "8 bit interleave" );
		}
		else
		{
			m_debug.n_skip++;

			if( m_debug.n_skip > 15 )
				m_debug.n_skip = 0;

			popmessage( "debug skip %d", m_debug.n_skip );
		}
	}

#if 0
	if( machine().input().code_pressed_once( KEYCODE_D ) )
	{
		FILE *f = fopen( "dump.txt", "w" );
		for( int n_y = 256; n_y < 512; n_y++ )
			for( int n_x = 640; n_x < 1024; n_x++ )
				fprintf( f, "%04u,%04u = %04x\n", n_y, n_x, p_p_vram[ n_y ][ n_x ] );
		fclose( f );
	}
	if( machine().input().code_pressed_once( KEYCODE_S ) )
	{
		popmessage( "saving..." );

		FILE *f = fopen( "VRAM.BIN", "wb" );
		for( int n_y = 0; n_y < 1024; n_y++ )
			fwrite( p_p_vram[ n_y ], 1024 * 2, 1, f );
		fclose( f );
	}
	if( machine().input().code_pressed_once( KEYCODE_L ) )
	{
		popmessage( "loading..." );

		FILE *f = fopen( "VRAM.BIN", "rb" );
		for( int n_y = 0; n_y < 1024; n_y++ )
			fread( p_p_vram[ n_y ], 1024 * 2, 1, f );
		fclose( f );
	}
#endif
}

int psxgpu_device::DebugMeshDisplay( bitmap_rgb32 &bitmap, const rectangle &cliprect )
{
	if( m_debug.b_mesh )
	{
		for( int y = cliprect.min_y; y <= cliprect.max_y; y++ )
			draw_scanline16( bitmap, cliprect.min_x, y, cliprect.max_x + 1 - cliprect.min_x, &m_debug.mesh->pix( y ), pens() );
	}

	m_debug.b_clear = 1;
	return m_debug.b_mesh;
}

int psxgpu_device::DebugTextureDisplay( bitmap_rgb32 &bitmap )
{
	if( m_debug.b_texture )
	{
		int width = screen().width();
		int height = screen().height();

		for( int n_y = 0; n_y < height; n_y++ )
		{
			uint16_t p_n_interleave[ 1024 ];

			for( int n_x = 0; n_x < width; n_x++ )
			{
				int n_xi;
				int n_yi;

				if( m_debug.n_interleave == 0 )
				{
					n_xi = ( n_x & ~0x3c ) + ( ( n_y << 2 ) & 0x3c );
					n_yi = ( n_y & ~0xf ) + ( ( n_x >> 2 ) & 0xf );
				}
				else if( m_debug.n_interleave == 1 )
				{
					n_xi = ( n_x & ~0x78 ) + ( ( n_x << 3 ) & 0x40 ) + ( ( n_y << 3 ) & 0x38 );
					n_yi = ( n_y & ~0x7 ) + ( ( n_x >> 4 ) & 0x7 );
				}
				else
				{
					n_xi = n_x;
					n_yi = n_y;
				}

				p_n_interleave[ n_x ] = p_p_vram[ n_yi ][ n_xi ];
			}

			draw_scanline16( bitmap, 0, n_y, width, p_n_interleave, pens() );
		}
	}

	return m_debug.b_texture;
}

#endif

void psxgpu_device::updatevisiblearea()
{
	rectangle visarea;
	double refresh;

	if( ( n_gpustatus & ( 1 << 0x14 ) ) != 0 )
	{
		/* pal */
		refresh = 50; // TODO: it's not exactly 50Hz
		switch( ( n_gpustatus >> 0x13 ) & 1 )
		{
		case 0:
			n_screenheight = 256;
			break;
		case 1:
			n_screenheight = 512;
			break;
		}
	}
	else
	{
		/* ntsc */
		// refresh rate derived from 53.693175MHz
		// TODO: emulate display timings at lower level
		switch( ( n_gpustatus >> 0x13 ) & 1 )
		{
		case 0:
			refresh = 59.8260978565;
			n_screenheight = 240;
			break;
		case 1:
			refresh = 59.9400523286;
			n_screenheight = 480;
			break;
		}
	}
	switch( ( n_gpustatus >> 0x11 ) & 3 )
	{
	case 0:
		switch( ( n_gpustatus >> 0x10 ) & 1 )
		{
		case 0:
			n_screenwidth = 256;
			break;
		case 1:
			n_screenwidth = 368;
			break;
		}
		break;
	case 1:
		switch( ( n_gpustatus >> 0x10 ) & 1 )
		{
		case 0:
			n_screenwidth = 320;
			break;
		case 1:
			n_screenwidth = 384;
			break;
		}
		break;
	case 2:
		n_screenwidth = 512;
		break;
	case 3:
		n_screenwidth = 640;
		break;
	}

#if PSXGPU_DEBUG_VIEWER
	if( m_debug.b_mesh || m_debug.b_texture )
	{
		n_screenheight = 1024;
		n_screenwidth = 1024;
	}
#endif

	visarea.set(0, n_screenwidth - 1, 0, n_screenheight - 1);
	screen().configure(n_screenwidth, n_screenheight, visarea, attotime::from_hz(refresh));
}

void psxgpu_device::psx_gpu_init( int n_gputype )
{
	int width = 1024;
	int height = ( m_ram->size() / width ) / sizeof( uint16_t );

	m_n_gputype = n_gputype;

#if PSXGPU_DEBUG_VIEWER
	DebugMeshInit();
#endif

	n_gpustatus = 0x14802000;
	n_gpuinfo = 0;
	n_gpu_buffer_offset = 0;
	n_lightgun_x = 0;
	n_lightgun_y = 0;
	b_reverseflag = 0;

	uint16_t* p_vram = m_ram->pointer<uint16_t>();

	for( int n_line = 0; n_line < 1024; n_line++ )
	{
		p_p_vram[ n_line ] = &p_vram[ ( n_line % height ) * width ];
	}

	for( int n_level = 0; n_level < MAX_LEVEL; n_level++ )
	{
		for( int n_shade = 0; n_shade < MAX_SHADE; n_shade++ )
		{
			/* shaded */
			int n_shaded = ( n_level * n_shade ) / MID_SHADE;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_redshade[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;
			p_n_greenshade[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded << 5;
			p_n_blueshade[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded << 10;

			/* 1/4 x transparency */
			n_shaded = ( n_level * n_shade ) / MID_SHADE;
			n_shaded >>= 2;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_f025[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;

			/* 1/2 x transparency */
			n_shaded = ( n_level * n_shade ) / MID_SHADE;
			n_shaded >>= 1;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_f05[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;

			/* 1 x transparency */
			n_shaded = ( n_level * n_shade ) / MID_SHADE;
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_f1[ ( n_level * MAX_SHADE ) | n_shade ] = n_shaded;
		}
	}

	for( int n_level = 0; n_level < 0x10000; n_level++ )
	{
		p_n_redlevel[ n_level ] = ( n_level & ( MAX_LEVEL - 1 ) ) * MAX_SHADE;
		p_n_greenlevel[ n_level ] = ( ( n_level >> 5 ) & ( MAX_LEVEL - 1 ) ) * MAX_SHADE;
		p_n_bluelevel[ n_level ] = ( ( n_level >> 10 ) & ( MAX_LEVEL - 1 ) ) * MAX_SHADE;

		/* 0.5 * background */
		p_n_redb05[ n_level ] = ( ( n_level & ( MAX_LEVEL - 1 ) ) / 2 ) * MAX_LEVEL;
		p_n_greenb05[ n_level ] = ( ( ( n_level >> 5 ) & ( MAX_LEVEL - 1 ) ) / 2 ) * MAX_LEVEL;
		p_n_blueb05[ n_level ] = ( ( ( n_level >> 10 ) & ( MAX_LEVEL - 1 ) ) / 2 ) * MAX_LEVEL;

		/* 1 * background */
		p_n_redb1[ n_level ] = ( n_level & ( MAX_LEVEL - 1 ) ) * MAX_LEVEL;
		p_n_greenb1[ n_level ] = ( ( n_level >> 5 ) & ( MAX_LEVEL - 1 ) ) * MAX_LEVEL;
		p_n_blueb1[ n_level ] = ( ( n_level >> 10 ) & ( MAX_LEVEL - 1 ) ) * MAX_LEVEL;

		/* 24bit color */
		p_n_g0r0[ n_level ] = ( ( ( n_level >> 8 ) & 0xff ) << 8 ) | ( ( ( n_level >> 0 ) & 0xff ) << 16 );
		p_n_b0[ n_level ] = ( ( n_level >> 0 ) & 0xff ) << 0;
		p_n_r1[ n_level ] = ( ( n_level >> 8 ) & 0xff ) << 16;
		p_n_b1g1[ n_level ] = ( ( ( n_level >> 8 ) & 0xff ) << 0 ) | ( ( ( n_level >> 0 ) & 0xff ) << 8 );
	}

	for( int n_level = 0; n_level < MAX_LEVEL; n_level++ )
	{
		for( int n_level2 = 0; n_level2 < MAX_LEVEL; n_level2++ )
		{
			/* add transparency */
			int n_shaded = ( n_level + n_level2 );
			if( n_shaded > MAX_LEVEL - 1 )
			{
				n_shaded = MAX_LEVEL - 1;
			}
			p_n_redaddtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded;
			p_n_greenaddtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 5;
			p_n_blueaddtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 10;

			/* sub transparency */
			n_shaded = ( n_level - n_level2 );
			if( n_shaded < 0 )
			{
				n_shaded = 0;
			}
			p_n_redsubtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded;
			p_n_greensubtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 5;
			p_n_bluesubtrans[ ( n_level * MAX_LEVEL ) | n_level2 ] = n_shaded << 10;
		}
	}

	save_item(NAME(m_packet.n_entry));
	save_item(NAME(n_gpu_buffer_offset));
	save_item(NAME(n_vramx));
	save_item(NAME(n_vramy));
	save_item(NAME(n_twy));
	save_item(NAME(n_twx));
	save_item(NAME(n_tww));
	save_item(NAME(n_drawarea_x1));
	save_item(NAME(n_drawarea_y1));
	save_item(NAME(n_drawarea_x2));
	save_item(NAME(n_drawarea_y2));
	save_item(NAME(n_horiz_disstart));
	save_item(NAME(n_horiz_disend));
	save_item(NAME(n_vert_disstart));
	save_item(NAME(n_vert_disend));
	save_item(NAME(b_reverseflag));
	save_item(NAME(n_drawoffset_x));
	save_item(NAME(n_drawoffset_y));
	save_item(NAME(m_n_displaystartx));
	save_item(NAME(n_displaystarty));
	save_item(NAME(n_gpustatus));
	save_item(NAME(n_gpuinfo));
	save_item(NAME(n_lightgun_x));
	save_item(NAME(n_lightgun_y));
	save_item(NAME(m_n_tx));
	save_item(NAME(m_n_ty));
	save_item(NAME(n_abr));
	save_item(NAME(n_tp));
	save_item(NAME(n_ix));
	save_item(NAME(n_iy));
	save_item(NAME(n_ti));
	save_item(NAME(m_draw_stp));
	save_item(NAME(m_check_stp));
}

void psxgpu_device::device_post_load()
{
	updatevisiblearea();
}

uint32_t psxgpu_device::update_screen(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	hk::prof::Scope hk_ps( hk::prof::GPU ); // HIRAKU
	uint32_t n_x;
	uint32_t n_y;
	int n_top;
	int n_line;
	int n_lines;
	int n_left;
	int n_column;
	int n_columns;
	int n_displaystartx;
	int n_overscantop;
	int n_overscanleft;

#if PSXGPU_DEBUG_VIEWER
	if( DebugMeshDisplay( bitmap, cliprect ) )
	{
		return 0;
	}
	if( DebugTextureDisplay( bitmap ) )
	{
		return 0;
	}
#endif

	if( ( n_gpustatus & ( 1 << 0x17 ) ) != 0 )
	{
		/* todo: only draw to necessary area */
		bitmap.fill(0, cliprect);
	}
	else
	{
		if( b_reverseflag )
		{
			n_displaystartx = ( 1023 - m_n_displaystartx );
			/* todo: make this flip the screen, in the meantime.. */
			n_displaystartx -= ( n_screenwidth - 1 );
		}
		else
		{
			n_displaystartx = m_n_displaystartx;
		}

		if( ( n_gpustatus & ( 1 << 0x14 ) ) != 0 )
		{
			/* pal */
			n_overscantop = 0x23;
			n_overscanleft = 0x27e;
		}
		else
		{
			/* ntsc */
			n_overscantop = 0x10;
			n_overscanleft = 0x260;
		}

		n_top = (int32_t)n_vert_disstart - n_overscantop;
		n_lines = (int32_t)n_vert_disend - (int32_t)n_vert_disstart;
		if( n_top < 0 )
		{
			n_y = -n_top;
			n_lines += n_top;
		}
		else
		{
			n_y = 0;

			/* draw top border */
			rectangle clip(cliprect.left(), cliprect.right(), cliprect.top(), n_top);
			bitmap.fill(0, clip);
		}
		if( ( n_gpustatus & ( 1 << 0x16 ) ) != 0 )
		{
			/* interlaced */
			n_lines *= 2;
		}
		if( n_lines > n_screenheight - ( n_y + n_top ) )
		{
			n_lines = n_screenheight - ( n_y + n_top );
		}
		else
		{
			/* draw bottom border */
			rectangle clip(cliprect.left(), cliprect.right(), n_y + n_top + n_lines, cliprect.bottom());
			bitmap.fill(0, clip);
		}

		n_left = ( ( (int32_t)n_horiz_disstart - n_overscanleft ) * (int32_t)n_screenwidth ) / 2560;
		n_columns = ( ( ( (int32_t)n_horiz_disend - n_horiz_disstart ) * (int32_t)n_screenwidth ) / 2560 );
		if( n_left < 0 )
		{
			n_x = -n_left;
			n_columns += n_left;
		}
		else
		{
			n_x = 0;

			/* draw left border */
			rectangle clip(cliprect.left(), n_x + n_left, cliprect.top(), cliprect.bottom());
			bitmap.fill(0, clip);
		}
		if( n_columns > n_screenwidth - ( n_x + n_left ) )
		{
			n_columns = n_screenwidth - ( n_x + n_left );
		}
		else
		{
			/* draw right border */
			rectangle clip(n_x + n_left + n_columns, cliprect.right(), cliprect.top(), cliprect.bottom());
			bitmap.fill(0, clip);
		}

		if( ( n_gpustatus & ( 1 << 0x15 ) ) != 0 )
		{
			/* 24bit */
			hk_sync(); // HIRAKU
			n_line = n_lines;
			while( n_line > 0 )
			{
				uint16_t *p_n_src = p_p_vram[ n_y + n_displaystarty ] + 3 * n_x + n_displaystartx;
				uint32_t *p_n_dest = &bitmap.pix(n_y + n_top, n_x + n_left);

				n_column = n_columns;
				while( n_column > 0 )
				{
					uint32_t n_g0r0 = *( p_n_src++ );
					uint32_t n_r1b0 = *( p_n_src++ );
					uint32_t n_b1g1 = *( p_n_src++ );

					*( p_n_dest++ ) = p_n_g0r0[ n_g0r0 ] | p_n_b0[ n_r1b0 ];
					n_column--;
					if( n_column > 0 )
					{
						*( p_n_dest++ ) = p_n_r1[ n_r1b0 ] | p_n_b1g1[ n_b1g1 ];
						n_column--;
					}
				}
				n_y++;
				n_line--;
			}
		}
		else
		{
			/* 15bit */
			hk_sync_area( n_x + n_displaystartx, n_columns, n_y + n_displaystarty, n_lines ); // HIRAKU: what it reads
			n_line = n_lines;
			while( n_line > 0 )
			{
				draw_scanline16( bitmap, n_x + n_left, n_y + n_top, n_columns, p_p_vram[ ( n_y + n_displaystarty ) & 1023 ] + n_x + n_displaystartx, pens() );
				n_y++;
				n_line--;
			}
		}
	}
	return 0;
}

#define WRITE_PIXEL( p ) \
	{ \
		if( !m_check_stp || ( *( p_vram ) & 0x8000 ) == 0 ) \
		{ \
			if( m_draw_stp ) \
				*( p_vram ) = ( p ) | 0x8000; \
			else \
				*( p_vram ) = p; \
		} \
	}

/*
type 1
f  e| d| c  b| a  9| 8  7| 6  5| 4| 3  2  1  0
    |ti|     |   tp|  abr|   ty|  |         tx
*/

/*
type 2
f  e| d  c| b| a  9| 8  7| 6  5| 4| 3  2  1  0
    |iy|ix|ty|     |   tp|  abr|ty|         tx
*/

void psxgpu_device::decode_tpage( uint32_t tpage )
{
	if( m_n_gputype == 2 )
	{
		n_gpustatus = ( n_gpustatus & 0xffff7800 ) | ( tpage & 0x7ff ) | ( ( tpage & 0x800 ) << 4 );

		m_n_tx = ( tpage & 0x0f ) << 6;
		m_n_ty = ( ( tpage & 0x10 ) << 4 ) | ( ( tpage & 0x800 ) >> 2 );
		n_abr = ( tpage & 0x60 ) >> 5;
		n_tp = ( tpage & 0x180 ) >> 7;
		n_ix = ( tpage & 0x1000 ) >> 12;
		n_iy = ( tpage & 0x2000 ) >> 13;
		n_ti = 0;
		if( ( tpage & ~0x39ff ) != 0 )
		{
			LOG("not handled: draw mode %08x\n", tpage & ~0x39ff);
		}
		if( n_tp == 3 )
		{
			logerror("not handled: tp == 3\n");
		}
	}
	else
	{
		// TODO: confirm status bits on real type 1 gpu
		n_gpustatus = ( n_gpustatus & 0xffffe000 ) | ( tpage & 0x1fff );

		m_n_tx = ( tpage & 0x0f ) << 6;
		m_n_ty = ( ( tpage & 0x60 ) << 3 );
		n_abr = ( tpage & 0x180 ) >> 7;
		n_tp = ( tpage & 0x600 ) >> 9;
		n_ti = ( tpage & 0x2000 ) >> 13;
		n_ix = 0;
		n_iy = 0;
		if( ( tpage & ~0x27ef ) != 0 )
		{
			LOG("not handled: draw mode %08x\n", tpage & ~0x27ef);
		}
		if( n_tp == 3 )
		{
			logerror("not handled: tp == 3\n");
		}
		else if( n_tp == 2 && n_ti != 0 )
		{
			logerror("not handled: interleaved 15 bit texture\n");
		}
	}
}

/* HIRAKU: the same, for raster */
void psxgpu_device::raster::decode_tpage( uint32_t tpage )
{
	if( m_n_gputype == 2 )
	{
		n_gpustatus = ( n_gpustatus & 0xffff7800 ) | ( tpage & 0x7ff ) | ( ( tpage & 0x800 ) << 4 );

		m_n_tx = ( tpage & 0x0f ) << 6;
		m_n_ty = ( ( tpage & 0x10 ) << 4 ) | ( ( tpage & 0x800 ) >> 2 );
		n_abr = ( tpage & 0x60 ) >> 5;
		n_tp = ( tpage & 0x180 ) >> 7;
		n_ix = ( tpage & 0x1000 ) >> 12;
		n_iy = ( tpage & 0x2000 ) >> 13;
		n_ti = 0;
		if( ( tpage & ~0x39ff ) != 0 )
		{
			LOG("not handled: draw mode %08x\n", tpage & ~0x39ff);
		}
		if( n_tp == 3 )
		{
			logerror("not handled: tp == 3\n");
		}
	}
	else
	{
		// TODO: confirm status bits on real type 1 gpu
		n_gpustatus = ( n_gpustatus & 0xffffe000 ) | ( tpage & 0x1fff );

		m_n_tx = ( tpage & 0x0f ) << 6;
		m_n_ty = ( ( tpage & 0x60 ) << 3 );
		n_abr = ( tpage & 0x180 ) >> 7;
		n_tp = ( tpage & 0x600 ) >> 9;
		n_ti = ( tpage & 0x2000 ) >> 13;
		n_ix = 0;
		n_iy = 0;
		if( ( tpage & ~0x27ef ) != 0 )
		{
			LOG("not handled: draw mode %08x\n", tpage & ~0x27ef);
		}
		if( n_tp == 3 )
		{
			logerror("not handled: tp == 3\n");
		}
		else if( n_tp == 2 && n_ti != 0 )
		{
			logerror("not handled: interleaved 15 bit texture\n");
		}
	}
}

#define SPRITESETUP \
	int n_dv; \
	if( n_iy != 0 ) \
	{ \
		n_dv = -1; \
	} \
	else \
	{ \
		n_dv = 1; \
	} \
	int n_du; \
	if( n_ix != 0 ) \
	{ \
		n_du = -1; \
	} \
	else \
	{ \
		n_du = 1; \
	}

#define TRANSPARENCYSETUP \
	uint16_t *p_n_f = p_n_f1; \
	uint16_t *p_n_redb = p_n_redb1; \
	uint16_t *p_n_greenb = p_n_greenb1; \
	uint16_t *p_n_blueb = p_n_blueb1; \
	uint16_t *p_n_redtrans = p_n_redaddtrans; \
	uint16_t *p_n_greentrans = p_n_greenaddtrans; \
	uint16_t *p_n_bluetrans = p_n_blueaddtrans; \
	\
	switch( n_cmd & 0x02 ) \
	{ \
	case 0x02: \
		switch( n_abr ) \
		{ \
		case 0x00: \
			p_n_f = p_n_f05; \
			p_n_redb = p_n_redb05; \
			p_n_greenb = p_n_greenb05; \
			p_n_blueb = p_n_blueb05; \
			p_n_redtrans = p_n_redaddtrans; \
			p_n_greentrans = p_n_greenaddtrans; \
			p_n_bluetrans = p_n_blueaddtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 0.5*B + 0.5*F\n"); \
			break; \
		case 0x01: \
			p_n_f = p_n_f1; \
			p_n_redb = p_n_redb1; \
			p_n_greenb = p_n_greenb1; \
			p_n_blueb = p_n_blueb1; \
			p_n_redtrans = p_n_redaddtrans; \
			p_n_greentrans = p_n_greenaddtrans; \
			p_n_bluetrans = p_n_blueaddtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 1.0*B + 1.0*F\n"); \
			break; \
		case 0x02: \
			p_n_f = p_n_f1; \
			p_n_redb = p_n_redb1; \
			p_n_greenb = p_n_greenb1; \
			p_n_blueb = p_n_blueb1; \
			p_n_redtrans = p_n_redsubtrans; \
			p_n_greentrans = p_n_greensubtrans; \
			p_n_bluetrans = p_n_bluesubtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 1.0*B - 1.0*F\n"); \
			break; \
		case 0x03: \
			p_n_f = p_n_f025; \
			p_n_redb = p_n_redb1; \
			p_n_greenb = p_n_greenb1; \
			p_n_blueb = p_n_blueb1; \
			p_n_redtrans = p_n_redaddtrans; \
			p_n_greentrans = p_n_greenaddtrans; \
			p_n_bluetrans = p_n_blueaddtrans; \
			LOGMASKED(LOG_TRANSPARENCY, "Transparency Mode: 1.0*B + 0.25*F\n"); \
			break; \
		} \
		break; \
	}

#define SOLIDSETUP \
	TRANSPARENCYSETUP

#define TEXTURESETUP \
	int n_tx = m_n_tx; \
	int n_ty = m_n_ty; \
	uint16_t *p_clut = p_p_vram[ n_cluty ] + n_clutx; \
	switch( n_tp ) \
	{ \
	case 0: \
		n_tx += n_twx >> 2; \
		n_ty += n_twy; \
		break; \
	case 1: \
		n_tx += n_twx >> 1; \
		n_ty += n_twy; \
		break; \
	case 2: \
		n_tx += n_twx >> 0; \
		n_ty += n_twy; \
		break; \
	} \
	TRANSPARENCYSETUP

#define FLATPOLYGONUPDATE
#define FLATRECTANGEUPDATE
#define GOURAUDPOLYGONUPDATE \
	n_r.d += n_dr; \
	n_g.d += n_dg; \
	n_b.d += n_db;

#define SOLIDFILL( PIXELUPDATE ) \
	if( n_distance > ( (int32_t)n_drawarea_x2 - drawx ) + 1 ) \
	{ \
		n_distance = ( n_drawarea_x2 - drawx ) + 1; \
	} \
	uint16_t *p_vram = p_p_vram[ drawy ] + drawx; \
	\
	switch( n_cmd & 0x02 ) \
	{ \
	case 0x00: \
		/* transparency off */ \
		while( n_distance > 0 ) \
		{ \
			WRITE_PIXEL( \
				p_n_redshade[ MID_LEVEL | n_r.w.h ] | \
				p_n_greenshade[ MID_LEVEL | n_g.w.h ] | \
				p_n_blueshade[ MID_LEVEL | n_b.w.h ] ) \
			p_vram++; \
			PIXELUPDATE \
			n_distance--; \
		} \
		break; \
	case 0x02: \
		/* transparency on */ \
		while( n_distance > 0 ) \
		{ \
			WRITE_PIXEL( \
				p_n_redtrans[ p_n_f[ MID_LEVEL | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] | \
				p_n_greentrans[ p_n_f[ MID_LEVEL | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] | \
				p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] ) \
			p_vram++; \
			PIXELUPDATE \
			n_distance--; \
		} \
		break; \
	}

#define FLATTEXTUREDPOLYGONUPDATE \
	n_u.d += n_du; \
	n_v.d += n_dv;

#define GOURAUDTEXTUREDPOLYGONUPDATE \
	n_r.d += n_dr; \
	n_g.d += n_dg; \
	n_b.d += n_db; \
	n_u.d += n_du; \
	n_v.d += n_dv;

#define FLATTEXTUREDRECTANGLEUPDATE \
	n_u += n_du;

#define TEXTURE_LOOP \
	while( n_distance > 0 ) \
	{
#define TEXTURE_ENDLOOP \
	}

#define TEXTURE4BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + TXV ] + n_tx + ( TXU >> 2 ) ) >> ( ( TXU & 0x03 ) << 2 ) ) & 0x0f ];

#define TEXTURE8BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + TXV ] + n_tx + ( TXU >> 1 ) ) >> ( ( TXU & 0x01 ) << 3 ) ) & 0xff ];

#define TEXTURE15BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		uint16_t n_bgr = *( p_p_vram[ n_ty + TXV ] + n_tx + TXU );

#define TEXTUREWINDOW4BIT( TXV, TXU ) TEXTURE4BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOW8BIT( TXV, TXU ) TEXTURE8BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOW15BIT( TXV, TXU ) TEXTURE15BIT( ( TXV & n_twh ), ( TXU & n_tww ) )

#define TEXTUREINTERLEAVED4BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		int n_xi = ( ( TXU >> 2 ) & ~0x3c ) + ( ( TXV << 2 ) & 0x3c ); \
		int n_yi = ( TXV & ~0xf ) + ( ( TXU >> 4 ) & 0xf ); \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + n_yi ] + n_tx + n_xi ) >> ( ( TXU & 0x03 ) << 2 ) ) & 0x0f ];

#define TEXTUREINTERLEAVED8BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		int n_xi = ( ( TXU >> 1 ) & ~0x78 ) + ( ( TXU << 2 ) & 0x40 ) + ( ( TXV << 3 ) & 0x38 ); \
		int n_yi = ( TXV & ~0x7 ) + ( ( TXU >> 5 ) & 0x7 ); \
		uint16_t n_bgr = p_clut[ ( *( p_p_vram[ n_ty + n_yi ] + n_tx + n_xi ) >> ( ( TXU & 0x01 ) << 3 ) ) & 0xff ];

#define TEXTUREINTERLEAVED15BIT( TXV, TXU ) \
	TEXTURE_LOOP \
		int n_xi = TXU; \
		int n_yi = TXV; \
		uint16_t n_bgr = *( p_p_vram[ n_ty + n_yi ] + n_tx + n_xi );

#define TEXTUREWINDOWINTERLEAVED4BIT( TXV, TXU ) TEXTUREINTERLEAVED4BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOWINTERLEAVED8BIT( TXV, TXU ) TEXTUREINTERLEAVED8BIT( ( TXV & n_twh ), ( TXU & n_tww ) )
#define TEXTUREWINDOWINTERLEAVED15BIT( TXV, TXU ) TEXTUREINTERLEAVED15BIT( ( TXV & n_twh ), ( TXU & n_tww ) )

/* HIRAKU: p_n_redlevel[] and friends, computed */
#define HK_RLEVEL( c ) ( ( ( c ) & 0x1f ) << 8 )
#define HK_GLEVEL( c ) ( ( ( ( c ) >> 5 ) & 0x1f ) << 8 )
#define HK_BLEVEL( c ) ( ( ( ( c ) >> 10 ) & 0x1f ) << 8 )
#define HK_NEUTRAL ( n_r.w.h == 0x80 && n_g.w.h == 0x80 && n_b.w.h == 0x80 )

#define SHADEDPIXEL( PIXELUPDATE ) \
		if( n_bgr != 0 ) \
		{ \
			if( HK_NEUTRAL ) \
				WRITE_PIXEL( n_bgr ) \
			else \
				WRITE_PIXEL( \
					p_n_redshade[ HK_RLEVEL( n_bgr ) | n_r.w.h ] | \
					p_n_greenshade[ HK_GLEVEL( n_bgr ) | n_g.w.h ] | \
					p_n_blueshade[ HK_BLEVEL( n_bgr ) | n_b.w.h ] | \
					( n_bgr & 0x8000 ) ) \
		} \
		p_vram++; \
		PIXELUPDATE \
		n_distance--; \
	TEXTURE_ENDLOOP

#define TRANSPARENTPIXEL( PIXELUPDATE ) \
		if( n_bgr != 0 ) \
		{ \
			if( ( n_bgr & 0x8000 ) != 0 ) \
			{ \
				WRITE_PIXEL( \
					p_n_redtrans[ p_n_f[ HK_RLEVEL( n_bgr ) | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] | \
					p_n_greentrans[ p_n_f[ HK_GLEVEL( n_bgr ) | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] | \
					p_n_bluetrans[ p_n_f[ HK_BLEVEL( n_bgr ) | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] | \
					0x8000 ) \
			} \
			else if( HK_NEUTRAL ) \
			{ \
				WRITE_PIXEL( n_bgr ) \
			} \
			else \
			{ \
				WRITE_PIXEL( \
					p_n_redshade[ HK_RLEVEL( n_bgr ) | n_r.w.h ] | \
					p_n_greenshade[ HK_GLEVEL( n_bgr ) | n_g.w.h ] | \
					p_n_blueshade[ HK_BLEVEL( n_bgr ) | n_b.w.h ] ) \
			} \
		} \
		p_vram++; \
		PIXELUPDATE \
		n_distance--; \
	TEXTURE_ENDLOOP

#define TEXTUREFILL( PIXELUPDATE, TXU, TXV ) \
	if( n_distance > ( (int32_t)n_drawarea_x2 - drawx ) + 1 ) \
	{ \
		n_distance = ( n_drawarea_x2 - drawx ) + 1; \
	} \
	uint16_t *p_vram = p_p_vram[ drawy ] + drawx; \
	\
	if( n_ti != 0 ) \
	{ \
		/* interleaved texture */ \
		if( n_twh != 255 || \
			n_tww != 255 || \
			n_twx != 0 || \
			n_twy != 0 ) \
		{ \
			/* texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOWINTERLEAVED4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOWINTERLEAVED8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOWINTERLEAVED15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOWINTERLEAVED4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOWINTERLEAVED8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOWINTERLEAVED15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
		else \
		{ \
			/* no texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREINTERLEAVED4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREINTERLEAVED8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREINTERLEAVED15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREINTERLEAVED4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREINTERLEAVED8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREINTERLEAVED15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
	} \
	else \
	{ \
		/* standard texture */ \
		if( n_twh != 255 || \
			n_tww != 255 || \
			n_twx != 0 || \
			n_twy != 0 ) \
		{ \
			/* texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOW4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOW8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOW15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTUREWINDOW4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTUREWINDOW8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTUREWINDOW15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
		else \
		{ \
			/* no texture window */ \
			switch( n_cmd & 0x02 ) \
			{ \
			case 0x00: \
				/* shading */ \
				switch( n_tp ) \
				{ \
				case 0: \
					TEXTURE4BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTURE8BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTURE15BIT( TXV, TXU ) \
					SHADEDPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			case 0x02: \
				/* semi transparency */ \
				switch( n_tp ) \
				{ \
				case 0: \
					/* 4 bit clut */ \
					TEXTURE4BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 1: \
					/* 8 bit clut */ \
					TEXTURE8BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				case 2: \
					/* 15 bit */ \
					TEXTURE15BIT( TXV, TXU ) \
					TRANSPARENTPIXEL( PIXELUPDATE ) \
					break; \
				} \
				break; \
			} \
		} \
	}

#define GET_COORD( a ) \
	a.sw.l = S11_COORD_X( a ); \
	a.sw.h = S11_COORD_Y( a );

static inline int CullVertex( int a, int b )
{
	int d = a - b;
	if( d < -1023 || d > 1023 )
	{
		return 1;
	}

	return 0;
}

#define CULLPOINT( PacketType, p1, p2 ) \
( \
	CullVertex( COORD_Y( m_packet.PacketType.vertex[ p1 ].n_coord ), COORD_Y( m_packet.PacketType.vertex[ p2 ].n_coord ) ) || \
	CullVertex( COORD_X( m_packet.PacketType.vertex[ p1 ].n_coord ), COORD_X( m_packet.PacketType.vertex[ p2 ].n_coord ) ) \
)

#define CULLTRIANGLE( PacketType, start ) \
( \
	CULLPOINT( PacketType, start, start + 1 ) || CULLPOINT( PacketType, start + 1, start + 2 ) || CULLPOINT( PacketType, start + 2, start ) \
)

#define FINDTOPLEFT( PacketType ) \
	for( int n_point = 0; n_point < n_points; n_point++ ) \
	{ \
		GET_COORD( m_packet.PacketType.vertex[ n_point ].n_coord ); \
	} \
	\
	const int *p_n_rightpointlist; \
	const int *p_n_leftpointlist; \
	int n_leftpoint = 0; \
	if( n_points == 4 ) \
	{ \
		if( CULLTRIANGLE( PacketType, 0 ) ) \
		{ \
			if( CULLTRIANGLE( PacketType, 1 ) ) \
			{ \
				return; \
			} \
			\
			p_n_rightpointlist = m_p_n_nextpointlist4b; \
			p_n_leftpointlist = m_p_n_prevpointlist4b; \
			n_leftpoint++; \
		} \
		else if( CULLTRIANGLE( PacketType, 1 ) ) \
		{ \
			p_n_rightpointlist = m_p_n_nextpointlist3; \
			p_n_leftpointlist = m_p_n_prevpointlist3; \
			n_points--; \
		} \
		else \
		{ \
			p_n_rightpointlist = m_p_n_nextpointlist4; \
			p_n_leftpointlist = m_p_n_prevpointlist4; \
		} \
	} \
	else if( CULLTRIANGLE( PacketType, 0 ) ) \
	{ \
		return; \
	} \
	else \
	{ \
		p_n_rightpointlist = m_p_n_nextpointlist3; \
		p_n_leftpointlist = m_p_n_prevpointlist3; \
	} \
	\
	for( int n_point = n_leftpoint + 1; n_point < n_points; n_point++ ) \
	{ \
		if( COORD_Y( m_packet.PacketType.vertex[ n_point ].n_coord ) < COORD_Y( m_packet.PacketType.vertex[ n_leftpoint ].n_coord ) || \
			( COORD_Y( m_packet.PacketType.vertex[ n_point ].n_coord ) == COORD_Y( m_packet.PacketType.vertex[ n_leftpoint ].n_coord ) && \
			COORD_X( m_packet.PacketType.vertex[ n_point ].n_coord ) < COORD_X( m_packet.PacketType.vertex[ n_leftpoint ].n_coord ) ) ) \
		{ \
			n_leftpoint = n_point; \
		} \
	} \
	int n_rightpoint = n_leftpoint;

void psxgpu_device::raster::FlatPolygon( int n_points )
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 1 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.FlatPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatPolygon.n_bgr );

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatPolygon.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatPolygon.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatPolygon.n_bgr ); n_b.w.l = 0;

	FINDTOPLEFT( FlatPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;

	int16_t n_y = COORD_Y( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.FlatPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.FlatPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATPOLYGONUPDATE )
		}

		n_cx1.d += n_dx1;
		n_cx2.d += n_dx2;
		n_y++;
	}
}

void psxgpu_device::raster::FlatTexturedPolygon( int n_points )
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 2 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatTexturedPolygon.n_bgr );

	uint32_t n_clutx = ( m_packet.FlatTexturedPolygon.vertex[ 0 ].n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.FlatTexturedPolygon.vertex[ 0 ].n_texture.w.h >> 6 ) & 0x3ff;

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cu1; n_cu1.d = 0;
	PAIR n_cv1; n_cv1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;
	PAIR n_cu2; n_cu2.d = 0;
	PAIR n_cv2; n_cv2.d = 0;

	decode_tpage( m_packet.FlatTexturedPolygon.vertex[ 1 ].n_texture.w.h );
	TEXTURESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.FlatTexturedPolygon.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.FlatTexturedPolygon.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.FlatTexturedPolygon.n_bgr ); n_b.w.l = 0;

	FINDTOPLEFT( FlatTexturedPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;
	int32_t n_du1 = 0;
	int32_t n_du2 = 0;
	int32_t n_dv1 = 0;
	int32_t n_dv2 = 0;

	int16_t n_y = COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_cu1.w.h = TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cu1.w.l = 0;
			n_cv1.w.h = TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cv1.w.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
			n_du1 = (int32_t)( ( TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cu1.d ) / n_distance;
			n_dv1 = (int32_t)( ( TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cv1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.FlatTexturedPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_cu2.w.h = TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cu2.w.l = 0;
			n_cv2.w.h = TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cv2.w.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
			n_du2 = (int32_t)( ( TEXTURE_U( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cu2.d ) / n_distance;
			n_dv2 = (int32_t)( ( TEXTURE_V( m_packet.FlatTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cv2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;
			PAIR n_u;
			PAIR n_v;
			int32_t n_du;
			int32_t n_dv;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;

				n_u.d = n_cu1.d;
				n_v.d = n_cv1.d;
				n_du = (int32_t)( n_cu2.d - n_cu1.d ) / n_distance;
				n_dv = (int32_t)( n_cv2.d - n_cv1.d ) / n_distance;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;

				n_u.d = n_cu2.d;
				n_v.d = n_cv2.d;
				n_du = (int32_t)( n_cu1.d - n_cu2.d ) / n_distance;
				n_dv = (int32_t)( n_cv1.d - n_cv2.d ) / n_distance;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u.d += n_du * ( n_drawarea_x1 - drawx );
				n_v.d += n_dv * ( n_drawarea_x1 - drawx );
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDPOLYGONUPDATE, n_u.w.h, n_v.w.h );
		}

		n_cx1.d += n_dx1;
		n_cu1.d += n_du1;
		n_cv1.d += n_dv1;
		n_cx2.d += n_dx2;
		n_cu2.d += n_du2;
		n_cv2.d += n_dv2;
		n_y++;
	}
}

void psxgpu_device::raster::GouraudPolygon( int n_points )
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 3 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.GouraudPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.GouraudPolygon.vertex[ 0 ].n_bgr );

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cr1; n_cr1.d = 0;
	PAIR n_cg1; n_cg1.d = 0;
	PAIR n_cb1; n_cb1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;
	PAIR n_cr2; n_cr2.d = 0;
	PAIR n_cg2; n_cg2.d = 0;
	PAIR n_cb2; n_cb2.d = 0;

	SOLIDSETUP

	FINDTOPLEFT( GouraudPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;
	int32_t n_dr1 = 0;
	int32_t n_dr2 = 0;
	int32_t n_dg1 = 0;
	int32_t n_dg2 = 0;
	int32_t n_db1 = 0;
	int32_t n_db2 = 0;

	int16_t n_y = COORD_Y( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_cr1.w.h = BGR_R( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ); n_cr1.w.l = 0;
			n_cg1.w.h = BGR_G( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ); n_cg1.w.l = 0;
			n_cb1.w.h = BGR_B( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ); n_cb1.w.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
			n_dr1 = (int32_t)( ( BGR_R( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cr1.d ) / n_distance;
			n_dg1 = (int32_t)( ( BGR_G( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cg1.d ) / n_distance;
			n_db1 = (int32_t)( ( BGR_B( m_packet.GouraudPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cb1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_cr2.w.h = BGR_R( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ); n_cr2.w.l = 0;
			n_cg2.w.h = BGR_G( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ); n_cg2.w.l = 0;
			n_cb2.w.h = BGR_B( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ); n_cb2.w.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
			n_dr2 = (int32_t)( ( BGR_R( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cr2.d ) / n_distance;
			n_dg2 = (int32_t)( ( BGR_G( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cg2.d ) / n_distance;
			n_db2 = (int32_t)( ( BGR_B( m_packet.GouraudPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cb2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;
			PAIR n_r;
			PAIR n_g;
			PAIR n_b;
			int32_t n_dr;
			int32_t n_dg;
			int32_t n_db;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;

				n_r.d = n_cr1.d;
				n_g.d = n_cg1.d;
				n_b.d = n_cb1.d;
				n_dr = (int32_t)( n_cr2.d - n_cr1.d ) / n_distance;
				n_dg = (int32_t)( n_cg2.d - n_cg1.d ) / n_distance;
				n_db = (int32_t)( n_cb2.d - n_cb1.d ) / n_distance;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;

				n_r.d = n_cr2.d;
				n_g.d = n_cg2.d;
				n_b.d = n_cb2.d;
				n_dr = (int32_t)( n_cr1.d - n_cr2.d ) / n_distance;
				n_dg = (int32_t)( n_cg1.d - n_cg2.d ) / n_distance;
				n_db = (int32_t)( n_cb1.d - n_cb2.d ) / n_distance;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_r.d += n_dr * ( n_drawarea_x1 - drawx );
				n_g.d += n_dg * ( n_drawarea_x1 - drawx );
				n_b.d += n_db * ( n_drawarea_x1 - drawx );
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( GOURAUDPOLYGONUPDATE )
		}

		n_cx1.d += n_dx1;
		n_cr1.d += n_dr1;
		n_cg1.d += n_dg1;
		n_cb1.d += n_db1;
		n_cx2.d += n_dx2;
		n_cr2.d += n_dr2;
		n_cg2.d += n_dg2;
		n_cb2.d += n_db2;
		n_y++;
	}
}

void psxgpu_device::raster::GouraudTexturedPolygon( int n_points )
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 4 )
	{
		return;
	}
	for( int n_point = 0; n_point < n_points; n_point++ )
	{
		DebugMesh( S11_COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_point ].n_coord ) + n_drawoffset_y );
	}
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_bgr );

	uint32_t n_clutx = ( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.GouraudTexturedPolygon.vertex[ 0 ].n_texture.w.h >> 6 ) & 0x3ff;

	PAIR n_cx1; n_cx1.d = 0;
	PAIR n_cr1; n_cr1.d = 0;
	PAIR n_cg1; n_cg1.d = 0;
	PAIR n_cb1; n_cb1.d = 0;
	PAIR n_cu1; n_cu1.d = 0;
	PAIR n_cv1; n_cv1.d = 0;
	PAIR n_cx2; n_cx2.d = 0;
	PAIR n_cr2; n_cr2.d = 0;
	PAIR n_cg2; n_cg2.d = 0;
	PAIR n_cb2; n_cb2.d = 0;
	PAIR n_cu2; n_cu2.d = 0;
	PAIR n_cv2; n_cv2.d = 0;

	decode_tpage( m_packet.GouraudTexturedPolygon.vertex[ 1 ].n_texture.w.h );
	TEXTURESETUP

	FINDTOPLEFT( GouraudTexturedPolygon )

	int32_t n_dx1 = 0;
	int32_t n_dx2 = 0;
	int32_t n_du1 = 0;
	int32_t n_du2 = 0;
	int32_t n_dr1 = 0;
	int32_t n_dr2 = 0;
	int32_t n_dg1 = 0;
	int32_t n_dg2 = 0;
	int32_t n_db1 = 0;
	int32_t n_db2 = 0;
	int32_t n_dv1 = 0;
	int32_t n_dv2 = 0;

	int16_t n_y = COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord );

	for( ;; )
	{
		if( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ p_n_leftpointlist[ n_leftpoint ] ].n_coord ) )
			{
				n_leftpoint = p_n_leftpointlist[ n_leftpoint ];
				if( n_leftpoint == n_rightpoint )
				{
					break;
				}
			}

			n_cx1.sw.h = COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ); n_cx1.sw.l = 0;
			n_cr1.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ); n_cr1.w.l = 0;
			n_cg1.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ); n_cg1.w.l = 0;
			n_cb1.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ); n_cb1.w.l = 0;
			n_cu1.w.h = TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cu1.w.l = 0;
			n_cv1.w.h = TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ); n_cv1.w.l = 0;
			n_leftpoint = p_n_leftpointlist[ n_leftpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx1 = (int32_t)( ( COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_coord ) << 16 ) - n_cx1.d ) / n_distance;
			n_dr1 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cr1.d ) / n_distance;
			n_dg1 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cg1.d ) / n_distance;
			n_db1 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_bgr ) << 16 ) - n_cb1.d ) / n_distance;
			n_du1 = (int32_t)( ( TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cu1.d ) / n_distance;
			n_dv1 = (int32_t)( ( TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_leftpoint ].n_texture ) << 16 ) - n_cv1.d ) / n_distance;
		}

		if( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ) )
		{
			while( n_y == COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ p_n_rightpointlist[ n_rightpoint ] ].n_coord ) )
			{
				n_rightpoint = p_n_rightpointlist[ n_rightpoint ];
				if( n_rightpoint == n_leftpoint )
				{
					break;
				}
			}

			n_cx2.sw.h = COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ); n_cx2.sw.l = 0;
			n_cr2.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ); n_cr2.w.l = 0;
			n_cg2.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ); n_cg2.w.l = 0;
			n_cb2.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ); n_cb2.w.l = 0;
			n_cu2.w.h = TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cu2.w.l = 0;
			n_cv2.w.h = TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ); n_cv2.w.l = 0;
			n_rightpoint = p_n_rightpointlist[ n_rightpoint ];

			int32_t n_distance = COORD_Y( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ) - n_y;
			if( n_distance < 1 )
			{
				break;
			}

			n_dx2 = (int32_t)( ( COORD_X( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_coord ) << 16 ) - n_cx2.d ) / n_distance;
			n_dr2 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_R( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cr2.d ) / n_distance;
			n_dg2 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_G( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cg2.d ) / n_distance;
			n_db2 = n_cmd & 0x01 ? 0 : (int32_t)( ( BGR_B( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_bgr ) << 16 ) - n_cb2.d ) / n_distance;
			n_du2 = (int32_t)( ( TEXTURE_U( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cu2.d ) / n_distance;
			n_dv2 = (int32_t)( ( TEXTURE_V( m_packet.GouraudTexturedPolygon.vertex[ n_rightpoint ].n_texture ) << 16 ) - n_cv2.d ) / n_distance;
		}

		int drawy = n_y + n_drawoffset_y;

		if( (int16_t)n_cx1.sw.h != (int16_t)n_cx2.sw.h && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int16_t n_x;
			int32_t n_distance;
			PAIR n_r;
			PAIR n_g;
			PAIR n_b;
			PAIR n_u;
			PAIR n_v;
			int32_t n_dr;
			int32_t n_dg;
			int32_t n_db;
			int32_t n_du;
			int32_t n_dv;

			if( (int16_t)n_cx1.sw.h < (int16_t)n_cx2.sw.h )
			{
				n_x = n_cx1.sw.h;
				n_distance = (int16_t)n_cx2.sw.h - n_x;

				n_r.d = n_cr1.d;
				n_g.d = n_cg1.d;
				n_b.d = n_cb1.d;
				n_u.d = n_cu1.d;
				n_v.d = n_cv1.d;
				n_dr = (int32_t)( n_cr2.d - n_cr1.d ) / n_distance;
				n_dg = (int32_t)( n_cg2.d - n_cg1.d ) / n_distance;
				n_db = (int32_t)( n_cb2.d - n_cb1.d ) / n_distance;
				n_du = (int32_t)( n_cu2.d - n_cu1.d ) / n_distance;
				n_dv = (int32_t)( n_cv2.d - n_cv1.d ) / n_distance;
			}
			else
			{
				n_x = n_cx2.sw.h;
				n_distance = (int16_t)n_cx1.sw.h - n_x;

				n_r.d = n_cr2.d;
				n_g.d = n_cg2.d;
				n_b.d = n_cb2.d;
				n_u.d = n_cu2.d;
				n_v.d = n_cv2.d;
				n_dr = (int32_t)( n_cr1.d - n_cr2.d ) / n_distance;
				n_dg = (int32_t)( n_cg1.d - n_cg2.d ) / n_distance;
				n_db = (int32_t)( n_cb1.d - n_cb2.d ) / n_distance;
				n_du = (int32_t)( n_cu1.d - n_cu2.d ) / n_distance;
				n_dv = (int32_t)( n_cv1.d - n_cv2.d ) / n_distance;
			}

			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_r.d += n_dr * ( n_drawarea_x1 - drawx );
				n_g.d += n_dg * ( n_drawarea_x1 - drawx );
				n_b.d += n_db * ( n_drawarea_x1 - drawx );
				n_u.d += n_du * ( n_drawarea_x1 - drawx );
				n_v.d += n_dv * ( n_drawarea_x1 - drawx );
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( GOURAUDTEXTUREDPOLYGONUPDATE, n_u.w.h, n_v.w.h );
		}

		n_cx1.d += n_dx1;
		n_cr1.d += n_dr1;
		n_cg1.d += n_dg1;
		n_cb1.d += n_db1;
		n_cu1.d += n_du1;
		n_cv1.d += n_dv1;
		n_cx2.d += n_dx2;
		n_cr2.d += n_dr2;
		n_cg2.d += n_dg2;
		n_cb2.d += n_db2;
		n_cu2.d += n_du2;
		n_cv2.d += n_dv2;
		n_y++;
	}
}

void psxgpu_device::raster::MonochromeLine()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 5 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.MonochromeLine.vertex[ 0 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.MonochromeLine.vertex[ 0 ].n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.MonochromeLine.vertex[ 1 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.MonochromeLine.vertex[ 1 ].n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	int32_t n_xstart = S11_COORD_X( m_packet.MonochromeLine.vertex[ 0 ].n_coord );
	int32_t n_xend = S11_COORD_X( m_packet.MonochromeLine.vertex[ 1 ].n_coord );
	int32_t n_ystart = S11_COORD_Y( m_packet.MonochromeLine.vertex[ 0 ].n_coord );
	int32_t n_yend = S11_COORD_Y( m_packet.MonochromeLine.vertex[ 1 ].n_coord );

	uint8_t n_cmd = BGR_C( m_packet.MonochromeLine.n_bgr );
	uint8_t n_r = BGR_R( m_packet.MonochromeLine.n_bgr );
	uint8_t n_g = BGR_G( m_packet.MonochromeLine.n_bgr );
	uint8_t n_b = BGR_B( m_packet.MonochromeLine.n_bgr );

	TRANSPARENCYSETUP

	int32_t n_xlen;
	if( n_xend > n_xstart )
	{
		n_xlen = n_xend - n_xstart;
	}
	else
	{
		n_xlen = n_xstart - n_xend;
	}

	int32_t n_ylen;
	if( n_yend > n_ystart )
	{
		n_ylen = n_yend - n_ystart;
	}
	else
	{
		n_ylen = n_ystart - n_yend;
	}

	int32_t n_len;
	if( n_xlen > n_ylen )
	{
		n_len = n_xlen;
	}
	else
	{
		n_len = n_ylen;
	}

	if( n_len == 0 )
	{
		n_len = 1;
	}

	PAIR n_x; n_x.sw.h = n_xstart; n_x.sw.l = 0;
	PAIR n_y; n_y.sw.h = n_ystart; n_y.sw.l = 0;

	int32_t n_dx = (int32_t)( ( n_xend << 16 ) - n_x.d ) / n_len;
	int32_t n_dy = (int32_t)( ( n_yend << 16 ) - n_y.d ) / n_len;

	while( n_len > 0 )
	{
		int drawx = n_x.sw.h + n_drawoffset_x;
		int drawy = n_y.sw.h + n_drawoffset_y;

		if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
			drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
		{
			uint16_t *p_vram = p_p_vram[ drawy ] + drawx;

			switch( n_cmd & 0x02 )
			{
			case 0x00:
				/* transparency off */
				WRITE_PIXEL(
					p_n_redshade[ MID_LEVEL | n_r ] |
					p_n_greenshade[ MID_LEVEL | n_g ] |
					p_n_blueshade[ MID_LEVEL | n_b ] )
				break;
			case 0x02:
				/* transparency on */
				WRITE_PIXEL(
					p_n_redtrans[ p_n_f[ MID_LEVEL | n_r ] | p_n_redb[ *( p_vram ) ] ] |
					p_n_greentrans[ p_n_f[ MID_LEVEL | n_g ] | p_n_greenb[ *( p_vram ) ] ] |
					p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b ] | p_n_blueb[ *( p_vram ) ] ] )
				break;
			}
		}

		n_x.d += n_dx;
		n_y.d += n_dy;
		n_len--;
	}
}

void psxgpu_device::raster::GouraudLine()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 6 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.GouraudLine.vertex[ 0 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudLine.vertex[ 0 ].n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.GouraudLine.vertex[ 1 ].n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.GouraudLine.vertex[ 1 ].n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.GouraudLine.vertex[ 0 ].n_bgr );

	TRANSPARENCYSETUP

	int32_t n_xstart = S11_COORD_X( m_packet.GouraudLine.vertex[ 0 ].n_coord );
	int32_t n_ystart = S11_COORD_Y( m_packet.GouraudLine.vertex[ 0 ].n_coord );
	PAIR n_cr1; n_cr1.w.h = BGR_R( m_packet.GouraudLine.vertex[ 0 ].n_bgr ); n_cr1.w.l = 0;
	PAIR n_cg1; n_cg1.w.h = BGR_G( m_packet.GouraudLine.vertex[ 0 ].n_bgr ); n_cg1.w.l = 0;
	PAIR n_cb1; n_cb1.w.h = BGR_B( m_packet.GouraudLine.vertex[ 0 ].n_bgr ); n_cb1.w.l = 0;

	int32_t n_xend = S11_COORD_X( m_packet.GouraudLine.vertex[ 1 ].n_coord );
	int32_t n_yend = S11_COORD_Y( m_packet.GouraudLine.vertex[ 1 ].n_coord );
	PAIR n_cr2; n_cr2.w.h = BGR_R( m_packet.GouraudLine.vertex[ 1 ].n_bgr ); n_cr2.w.l = 0;
	PAIR n_cg2; n_cg2.w.h = BGR_G( m_packet.GouraudLine.vertex[ 1 ].n_bgr ); n_cg2.w.l = 0;
	PAIR n_cb2; n_cb2.w.h = BGR_B( m_packet.GouraudLine.vertex[ 1 ].n_bgr ); n_cb2.w.l = 0;


	PAIR n_x; n_x.sw.h = n_xstart; n_x.sw.l = 0;
	PAIR n_y; n_y.sw.h = n_ystart; n_y.sw.l = 0;
	PAIR n_r; n_r.d = n_cr1.d;
	PAIR n_g; n_g.d = n_cg1.d;
	PAIR n_b; n_b.d = n_cb1.d;

	int32_t n_xlen;
	if( n_xend > n_xstart )
	{
		n_xlen = n_xend - n_xstart;
	}
	else
	{
		n_xlen = n_xstart - n_xend;
	}

	int32_t n_ylen;
	if( n_yend > n_ystart )
	{
		n_ylen = n_yend - n_ystart;
	}
	else
	{
		n_ylen = n_ystart - n_yend;
	}

	int32_t n_distance;
	if( n_xlen > n_ylen )
	{
		n_distance = n_xlen;
	}
	else
	{
		n_distance = n_ylen;
	}

	if( n_distance == 0 )
	{
		n_distance = 1;
	}

	int32_t n_dx = (int32_t)( ( n_xend << 16 ) - n_x.sd ) / n_distance;
	int32_t n_dy = (int32_t)( ( n_yend << 16 ) - n_y.sd ) / n_distance;
	int32_t n_dr = (int32_t)( n_cr2.d - n_cr1.d ) / n_distance;
	int32_t n_dg = (int32_t)( n_cg2.d - n_cg1.d ) / n_distance;
	int32_t n_db = (int32_t)( n_cb2.d - n_cb1.d ) / n_distance;

	while( n_distance > 0 )
	{
		int drawx = n_x.sw.h + n_drawoffset_x;
		int drawy = n_y.sw.h + n_drawoffset_y;

		if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
			drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
		{
			uint16_t *p_vram = p_p_vram[ drawy ] + drawx;

			switch( n_cmd & 0x02 )
			{
			case 0x00:
				/* transparency off */
				WRITE_PIXEL(
					p_n_redshade[ MID_LEVEL | n_r.w.h ] |
					p_n_greenshade[ MID_LEVEL | n_g.w.h ] |
					p_n_blueshade[ MID_LEVEL | n_b.w.h ] )
				break;
			case 0x02:
				/* transparency on */
				WRITE_PIXEL(
					p_n_redtrans[ p_n_f[ MID_LEVEL | n_r.w.h ] | p_n_redb[ *( p_vram ) ] ] |
					p_n_greentrans[ p_n_f[ MID_LEVEL | n_g.w.h ] | p_n_greenb[ *( p_vram ) ] ] |
					p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b.w.h ] | p_n_blueb[ *( p_vram ) ] ] )
				break;
			}
		}

		n_x.sd += n_dx;
		n_y.sd += n_dy;
		n_r.d += n_dr;
		n_g.d += n_dg;
		n_b.d += n_db;
		n_distance--;
	}
}

void psxgpu_device::raster::FrameBufferRectangleDraw()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 7 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMeshEnd();
#endif

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle.n_bgr ); n_b.w.l = 0;

	int16_t n_y = COORD_Y( m_packet.FlatRectangle.n_coord );
	int32_t n_h = SIZE_H( m_packet.FlatRectangle.n_size );

	while( n_h > 0 )
	{
		int16_t n_x = COORD_X( m_packet.FlatRectangle.n_coord );
		int32_t n_distance = SIZE_W( m_packet.FlatRectangle.n_size );

		while( n_distance > 0 )
		{
			p_p_vram[ n_y & 1023 ][ n_x & 1023 ] =
				p_n_redshade[ MID_LEVEL | n_r.w.h ] |
				p_n_greenshade[ MID_LEVEL | n_g.w.h ] |
				p_n_blueshade[ MID_LEVEL | n_b.w.h ];
			n_x++;
			n_distance--;
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::raster::FlatRectangle()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 8 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatRectangle.n_size ), S11_COORD_Y( m_packet.FlatRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatRectangle.n_size ) );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatRectangle.n_bgr );

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatRectangle.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatRectangle.n_coord );
	int32_t n_h = SIZE_H( m_packet.FlatRectangle.n_size );

	while( n_h > 0 )
	{
		int32_t n_distance = SIZE_W( m_packet.FlatRectangle.n_size );
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			if( ( n_cmd & 0x02 ) == 0 && !m_check_stp )
			{
				/* HIRAKU: opaque and no mask test: every pixel the same colour, filled at once */
				if( n_distance > ( (int32_t)n_drawarea_x2 - drawx ) + 1 )
					n_distance = ( n_drawarea_x2 - drawx ) + 1;
				if( n_distance > 0 )
					std::fill_n( p_p_vram[ drawy ] + drawx, n_distance, (uint16_t)( (
						p_n_redshade[ MID_LEVEL | n_r.w.h ] |
						p_n_greenshade[ MID_LEVEL | n_g.w.h ] |
						p_n_blueshade[ MID_LEVEL | n_b.w.h ] ) | ( m_draw_stp ? 0x8000 : 0 ) ) );
			}
			else
			{
				SOLIDFILL( FLATRECTANGEUPDATE )
			}
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::raster::FlatRectangle8x8()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 9 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x + 8, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y + 8 );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_x + 8, S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord ) + n_drawoffset_y + 8 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatRectangle8x8.n_bgr );

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle8x8.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle8x8.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle8x8.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatRectangle8x8.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatRectangle8x8.n_coord );
	int32_t n_h = 8;

	while( n_h > 0 )
	{
		int32_t n_distance = 8;
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATRECTANGEUPDATE )
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::raster::FlatRectangle16x16()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 10 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x + 16, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y + 16 );
	DebugMesh( S11_COORD_X( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_x + 16, S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord ) + n_drawoffset_y + 16 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatRectangle16x16.n_bgr );

	SOLIDSETUP

	PAIR n_r; n_r.w.h = BGR_R( m_packet.FlatRectangle16x16.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = BGR_G( m_packet.FlatRectangle16x16.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = BGR_B( m_packet.FlatRectangle16x16.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatRectangle16x16.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatRectangle16x16.n_coord );
	int32_t n_h = 16;

	while( n_h > 0 )
	{
		int32_t n_distance = 16;
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && n_y >= (int32_t)n_drawarea_y1 && n_y <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			SOLIDFILL( FLATRECTANGEUPDATE )
		}

		n_y++;
		n_h--;
	}
}

void psxgpu_device::raster::FlatTexturedRectangle()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 11 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatTexturedRectangle.n_size ), S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatTexturedRectangle.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_x + SIZE_W( m_packet.FlatTexturedRectangle.n_size ), S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord ) + n_drawoffset_y + SIZE_H( m_packet.FlatTexturedRectangle.n_size ) );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.FlatTexturedRectangle.n_bgr );

	uint32_t n_clutx = ( m_packet.FlatTexturedRectangle.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.FlatTexturedRectangle.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP
	SPRITESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.FlatTexturedRectangle.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.FlatTexturedRectangle.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.FlatTexturedRectangle.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.FlatTexturedRectangle.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.FlatTexturedRectangle.n_coord );
	uint8_t n_v = TEXTURE_V( m_packet.FlatTexturedRectangle.n_texture );
	uint32_t n_h = SIZE_H( m_packet.FlatTexturedRectangle.n_size );

	while( n_h > 0 )
	{
		uint8_t n_u = TEXTURE_U( m_packet.FlatTexturedRectangle.n_texture );
		int16_t n_distance = SIZE_W( m_packet.FlatTexturedRectangle.n_size );
		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u += ( n_drawarea_x1 - drawx ) * n_du;
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDRECTANGLEUPDATE, n_u, n_v );
		}

		n_v += n_dv;
		n_y++;
		n_h--;
	}
}

void psxgpu_device::raster::Sprite8x8()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 12 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y + 7 );
	DebugMesh( S11_COORD_X( m_packet.Sprite8x8.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite8x8.n_coord ) + n_drawoffset_y + 7 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.Sprite8x8.n_bgr );

	uint32_t n_clutx = ( m_packet.Sprite8x8.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.Sprite8x8.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP
	SPRITESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.Sprite8x8.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.Sprite8x8.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.Sprite8x8.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.Sprite8x8.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.Sprite8x8.n_coord );
	uint8_t n_v = TEXTURE_V( m_packet.Sprite8x8.n_texture );
	uint32_t n_h = 8;

	while( n_h > 0 )
	{
		uint8_t n_u = TEXTURE_U( m_packet.Sprite8x8.n_texture );
		int16_t n_distance = 8;

		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u += ( n_drawarea_x1 - drawx ) * n_du;
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDRECTANGLEUPDATE, n_u, n_v );
		}

		n_v += n_dv;
		n_y++;
		n_h--;
	}
}

void psxgpu_device::raster::Sprite16x16()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 13 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y );
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y + 7 );
	DebugMesh( S11_COORD_X( m_packet.Sprite16x16.n_coord ) + n_drawoffset_x + 7, S11_COORD_Y( m_packet.Sprite16x16.n_coord ) + n_drawoffset_y + 7 );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.Sprite16x16.n_bgr );

	uint32_t n_clutx = ( m_packet.Sprite16x16.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.Sprite16x16.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP
	SPRITESETUP

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.Sprite16x16.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.Sprite16x16.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.Sprite16x16.n_bgr ); n_b.w.l = 0;

	int16_t n_x = S11_COORD_X( m_packet.Sprite16x16.n_coord );
	int16_t n_y = S11_COORD_Y( m_packet.Sprite16x16.n_coord );
	uint8_t n_v = TEXTURE_V( m_packet.Sprite16x16.n_texture );
	uint32_t n_h = 16;

	while( n_h > 0 )
	{
		uint8_t n_u = TEXTURE_U( m_packet.Sprite16x16.n_texture );
		int16_t n_distance = 16;

		int drawy = n_y + n_drawoffset_y;

		if( n_distance > 0 && drawy >= (int32_t)n_drawarea_y1 && drawy <= (int32_t)n_drawarea_y2 )
		{
			int drawx = n_x + n_drawoffset_x;

			if( ( (int32_t)n_drawarea_x1 - drawx ) > 0 )
			{
				n_u += ( n_drawarea_x1 - drawx ) * n_du;
				n_distance -= ( n_drawarea_x1 - drawx );
				drawx = n_drawarea_x1;
			}

			TEXTUREFILL( FLATTEXTUREDRECTANGLEUPDATE, n_u, n_v );
		}

		n_v += n_dv;
		n_y++;
		n_h--;
	}
}

void psxgpu_device::raster::Dot()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 14 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.Dot.vertex.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.Dot.vertex.n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.Dot.n_bgr );
	uint8_t n_r = BGR_R( m_packet.Dot.n_bgr );
	uint8_t n_g = BGR_G( m_packet.Dot.n_bgr );
	uint8_t n_b = BGR_B( m_packet.Dot.n_bgr );
	int32_t n_x = S11_COORD_X( m_packet.Dot.vertex.n_coord );
	int32_t n_y = S11_COORD_Y( m_packet.Dot.vertex.n_coord );

	TRANSPARENCYSETUP

	int drawx = n_x + n_drawoffset_x;
	int drawy = n_y + n_drawoffset_y;

	if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
		drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
	{
		uint16_t *p_vram = p_p_vram[ drawy ] + drawx;

		switch( n_cmd & 0x02 )
		{
		case 0x00:
			/* transparency off */
			WRITE_PIXEL(
				p_n_redshade[ MID_LEVEL | n_r ] |
				p_n_greenshade[ MID_LEVEL | n_g ] |
				p_n_blueshade[ MID_LEVEL | n_b ] )
			break;
		case 0x02:
			/* transparency on */
			WRITE_PIXEL(
				p_n_redtrans[ p_n_f[ MID_LEVEL | n_r ] | p_n_redb[ *( p_vram ) ] ] |
				p_n_greentrans[ p_n_f[ MID_LEVEL | n_g ] | p_n_greenb[ *( p_vram ) ] ] |
				p_n_bluetrans[ p_n_f[ MID_LEVEL | n_b ] | p_n_blueb[ *( p_vram ) ] ] )
			break;
		}
	}
}

void psxgpu_device::raster::TexturedDot()
{
#if PSXGPU_DEBUG_VIEWER
	if (m_debug.n_skip == 15)
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.TexturedDot.vertex.n_coord ) + n_drawoffset_x, S11_COORD_Y( m_packet.TexturedDot.vertex.n_coord ) + n_drawoffset_y );
	DebugMeshEnd();
#endif

	uint8_t n_cmd = BGR_C( m_packet.TexturedDot.n_bgr );

	PAIR n_r; n_r.w.h = n_cmd & 0x01 ? 0x80 : BGR_R( m_packet.TexturedDot.n_bgr ); n_r.w.l = 0;
	PAIR n_g; n_g.w.h = n_cmd & 0x01 ? 0x80 : BGR_G( m_packet.TexturedDot.n_bgr ); n_g.w.l = 0;
	PAIR n_b; n_b.w.h = n_cmd & 0x01 ? 0x80 : BGR_B( m_packet.TexturedDot.n_bgr ); n_b.w.l = 0;

	int32_t n_x = S11_COORD_X( m_packet.TexturedDot.vertex.n_coord );
	int32_t n_y = S11_COORD_Y( m_packet.TexturedDot.vertex.n_coord );
	uint8_t n_u = TEXTURE_U(m_packet.TexturedDot.vertex.n_texture );
	uint8_t n_v = TEXTURE_V(m_packet.TexturedDot.vertex.n_texture );
	uint32_t n_clutx = ( m_packet.TexturedDot.vertex.n_texture.w.h & 0x3f ) << 4;
	uint32_t n_cluty = ( m_packet.TexturedDot.vertex.n_texture.w.h >> 6 ) & 0x3ff;

	TEXTURESETUP

	int32_t n_distance = 1;

	int drawx = n_x + n_drawoffset_x;
	int drawy = n_y + n_drawoffset_y;

	if( drawx >= (int32_t)n_drawarea_x1 && drawy >= (int32_t)n_drawarea_y1 &&
		drawx <= (int32_t)n_drawarea_x2 && drawy <= (int32_t)n_drawarea_y2 )
	{
		TEXTUREFILL( {}, n_u, n_v );
	}
}

void psxgpu_device::raster::MoveImage()
{
#if PSXGPU_DEBUG_VIEWER
	if( m_debug.n_skip == 16 )
	{
		return;
	}
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_W( m_packet.MoveImage.n_size ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) );
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_H( m_packet.MoveImage.n_size ) );
	DebugMesh( S11_COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_W( m_packet.MoveImage.n_size ), S11_COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord ) + SIZE_H( m_packet.MoveImage.n_size ) );
	DebugMeshEnd();
#endif

	int16_t n_srcy = COORD_Y( m_packet.MoveImage.vertex[ 0 ].n_coord );
	int16_t n_dsty = COORD_Y( m_packet.MoveImage.vertex[ 1 ].n_coord );
	int16_t n_h = SIZE_H( m_packet.MoveImage.n_size );

	while( n_h > 0 )
	{
		int16_t n_srcx = COORD_X( m_packet.MoveImage.vertex[ 0 ].n_coord );
		int16_t n_dstx = COORD_X( m_packet.MoveImage.vertex[ 1 ].n_coord );
		int16_t n_w = SIZE_W( m_packet.MoveImage.n_size );

		while( n_w > 0 )
		{
			uint16_t *p_vram = p_p_vram[ n_dsty & 1023 ] + ( n_dstx & 1023 );
			WRITE_PIXEL( *( p_p_vram[ n_srcy & 1023 ] + ( n_srcx & 1023 ) ) )
			n_srcx++;
			n_dstx++;
			n_w--;
		}

		n_srcy++;
		n_dsty++;
		n_h--;
	}
}

void psxgpu_device::dma_write( uint32_t *p_n_psxram, uint32_t n_address, int32_t n_size )
{
	gpu_write( &p_n_psxram[ n_address / 4 ], n_size );
}

void psxgpu_device::gpu_write( uint32_t *p_ram, int32_t n_size )
{
	hk::prof::Scope hk_ps( hk::prof::GPU ); // HIRAKU
	while( n_size > 0 )
	{
		uint32_t data = *( p_ram );

		if( m_hk_image != nullptr && ( n_gpu_buffer_offset != 3 || ( m_packet.n_entry[ 0 ] >> 24 ) != 0xa0 ) )
			hk_close(); // HIRAKU
		LOG("PSX Packet #%u %08x\n", n_gpu_buffer_offset, data);
		m_packet.n_entry[ n_gpu_buffer_offset ] = data;
		switch( m_packet.n_entry[ 0 ] >> 24 )
		{
		case 0x00:
			LOGMASKED(LOG_WRITE, "%s: not handled: GPU Command 0x00: (%08x)\n", machine().describe_context(), data);
			break;
		case 0x01:
			LOGMASKED(LOG_WRITE, "%s: not handled: clear cache\n", machine().describe_context());
			break;
		case 0x02:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: frame buffer rectangle %u,%u %u,%u\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 1 ] & 0xffff, m_packet.n_entry[ 1 ] >> 16, m_packet.n_entry[ 2 ] & 0xffff, m_packet.n_entry[ 2 ] >> 16 );
				FrameBufferRectangleDraw();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x20:
		case 0x21:
		case 0x22:
		case 0x23:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, machine().describe_context(), "%s: %02x: monochrome 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x24:
		case 0x25:
		case 0x26:
		case 0x27:
			if( n_gpu_buffer_offset < 6 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: textured 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatTexturedPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x28:
		case 0x29:
		case 0x2a:
		case 0x2b:
			if( n_gpu_buffer_offset < 4 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: monochrome 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x2c:
		case 0x2d:
		case 0x2e:
		case 0x2f:
			if( n_gpu_buffer_offset < 8 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: textured 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				FlatTexturedPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x30:
		case 0x31:
		case 0x32:
		case 0x33:
			if( n_gpu_buffer_offset < 5 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24 );
				GouraudPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x34:
		case 0x35:
		case 0x36:
		case 0x37:
			if( n_gpu_buffer_offset < 8 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud textured 3 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudTexturedPolygon( 3 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x38:
		case 0x39:
		case 0x3a:
		case 0x3b:
			if( n_gpu_buffer_offset < 7 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x3c:
		case 0x3d:
		case 0x3e:
		case 0x3f:
			if( n_gpu_buffer_offset < 11 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud textured 4 point polygon\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudTexturedPolygon( 4 );
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x40:
		case 0x41:
		case 0x42:
		case 0x43:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: monochrome line\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				MonochromeLine();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x48:
		case 0x4a:
		case 0x4c:
		case 0x4e:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: monochrome polyline\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				MonochromeLine();
				if( ( m_packet.n_entry[ 3 ] & 0xf000f000 ) != 0x50005000 )
				{
					m_packet.n_entry[ 1 ] = m_packet.n_entry[ 2 ];
					m_packet.n_entry[ 2 ] = m_packet.n_entry[ 3 ];
					n_gpu_buffer_offset = 3;
				}
				else
				{
					n_gpu_buffer_offset = 0;
				}
			}
			break;
		case 0x50:
		case 0x51:
		case 0x52:
		case 0x53:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud line\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudLine();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x58:
		case 0x5a:
		case 0x5c:
		case 0x5e:
			if( n_gpu_buffer_offset < 5 &&
				( n_gpu_buffer_offset != 4 || ( m_packet.n_entry[ 4 ] & 0xf000f000 ) != 0x50005000 ) )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: gouraud polyline\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				GouraudLine();
				if( ( m_packet.n_entry[ 4 ] & 0xf000f000 ) != 0x50005000 )
				{
					m_packet.n_entry[ 0 ] = ( m_packet.n_entry[ 0 ] & 0xff000000 ) | ( m_packet.n_entry[ 2 ] & 0x00ffffff );
					m_packet.n_entry[ 1 ] = m_packet.n_entry[ 3 ];
					m_packet.n_entry[ 2 ] = m_packet.n_entry[ 4 ];
					m_packet.n_entry[ 3 ] = m_packet.n_entry[ 5 ];
					n_gpu_buffer_offset = 4;
				}
				else
				{
					n_gpu_buffer_offset = 0;
				}
			}
			break;
		case 0x60:
		case 0x61:
		case 0x62:
		case 0x63:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: 02x: rectangle %d,%d %d,%d\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					(int16_t)( m_packet.n_entry[ 2 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 2 ] >> 16 ) );
				FlatRectangle();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x64:
		case 0x65:
		case 0x66:
		case 0x67:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: sprite %d,%d %u,%u %08x, %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					m_packet.n_entry[ 3 ] & 0xffff, m_packet.n_entry[ 3 ] >> 16,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 2 ] );
				FlatTexturedRectangle();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x68:
		case 0x69:
		case 0x6a:
		case 0x6b:
			if( n_gpu_buffer_offset < 1 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: dot %d,%d %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					m_packet.n_entry[ 0 ] & 0xffffff );
				Dot();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x6c:
		case 0x6d:
		case 0x6e:
		case 0x6f:
			if (n_gpu_buffer_offset < 2)
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: textured dot %d,%d %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ] >> 24,
					(int16_t)( m_packet.n_entry[ 1 ] & 0xffff ), (int16_t)( m_packet.n_entry[ 1 ] >> 16 ),
					m_packet.n_entry[ 0 ] & 0xffffff );
				TexturedDot();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x70:
		case 0x71:
		case 0x72:
		case 0x73:
			/* 8*8 rectangle */
			if( n_gpu_buffer_offset < 1 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s; %02x: 16x16 rectangle %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ] );
				FlatRectangle8x8();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x74:
		case 0x75:
		case 0x76:
		case 0x77:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: 8x8 sprite %08x %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ], m_packet.n_entry[ 2 ] );
				Sprite8x8();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x78:
		case 0x79:
		case 0x7a:
		case 0x7b:
			/* 16*16 rectangle */
			if( n_gpu_buffer_offset < 1 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: 16x16 rectangle %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ] );
				FlatRectangle16x16();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x7c:
		case 0x7d:
		case 0x7e:
		case 0x7f:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: 16x16 sprite %08x %08x %08x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ], m_packet.n_entry[ 2 ] );
				Sprite16x16();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0x80:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: move image in frame buffer %08x %08x %08x %08x\n", machine().describe_context(),
					m_packet.n_entry[ 0 ], m_packet.n_entry[ 1 ], m_packet.n_entry[ 2 ], m_packet.n_entry[ 3 ]);
				MoveImage();
				n_gpu_buffer_offset = 0;
			}
			break;
		case 0xa0:
			if( n_gpu_buffer_offset < 3 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				hk_image( data ); // HIRAKU: drawn by raster::image()
				for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
				{
					LOGMASKED(LOG_WRITE, "%s: send image to framebuffer ( pixel %u,%u = %u )\n",
						machine().describe_context(),
						( n_vramx + m_packet.n_entry[ 1 ] ) & 1023,
						( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023,
						data & 0xffff );

					n_vramx++;
					if( n_vramx >= ( m_packet.n_entry[ 2 ] & 0xffff ) )
					{
						n_vramx = 0;
						n_vramy++;
						if( n_vramy >= ( m_packet.n_entry[ 2 ] >> 16 ) )
						{
							LOGMASKED(LOG_WRITE, "%s: %02x: send image to framebuffer %u,%u %u,%u\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
								m_packet.n_entry[ 1 ] & 0xffff, ( m_packet.n_entry[ 1 ] >> 16 ),
								m_packet.n_entry[ 2 ] & 0xffff, ( m_packet.n_entry[ 2 ] >> 16 ) );
							n_gpu_buffer_offset = 0;
							n_vramx = 0;
							n_vramy = 0;
							break;
						}
					}
					data >>= 16;
				}
			}
			break;
		case 0xc0:
			if( n_gpu_buffer_offset < 2 )
			{
				n_gpu_buffer_offset++;
			}
			else
			{
				LOGMASKED(LOG_WRITE, "%s: %02x: copy image from frame buffer\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24);
				n_gpustatus |= ( 1L << 0x1b );
			}
			break;
		case 0xe1:
			LOGMASKED(LOG_WRITE, "%s: %02x: draw mode %06x\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				m_packet.n_entry[ 0 ] & 0xffffff );
			decode_tpage( m_packet.n_entry[ 0 ] & 0xffffff );
			break;
		case 0xe2:
			n_twy = ( ( ( m_packet.n_entry[ 0 ] >> 15 ) & 0x1f ) << 3 );
			n_twx = ( ( ( m_packet.n_entry[ 0 ] >> 10 ) & 0x1f ) << 3 );
			n_twh = 255 - ( ( ( m_packet.n_entry[ 0 ] >> 5 ) & 0x1f ) << 3 );
			n_tww = 255 - ( ( m_packet.n_entry[ 0 ] & 0x1f ) << 3 );
			LOGMASKED(LOG_WRITE, "%s: %02x: texture window %u,%u %u,%u\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_twx, n_twy, n_tww, n_twh );
			break;
		case 0xe3:
			n_drawarea_x1 = m_packet.n_entry[ 0 ] & 1023;
			if( m_n_gputype == 2 )
			{
				n_drawarea_y1 = ( m_packet.n_entry[ 0 ] >> 10 ) & 1023;
			}
			else
			{
				n_drawarea_y1 = ( m_packet.n_entry[ 0 ] >> 12 ) & 1023;
			}
			LOGMASKED(LOG_WRITE, "%s: %02x: drawing area top left %d,%d\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_drawarea_x1, n_drawarea_y1 );
			break;
		case 0xe4:
			n_drawarea_x2 = m_packet.n_entry[ 0 ] & 1023;
			if( m_n_gputype == 2 )
			{
				n_drawarea_y2 = ( m_packet.n_entry[ 0 ] >> 10 ) & 1023;
			}
			else
			{
				n_drawarea_y2 = ( m_packet.n_entry[ 0 ] >> 12 ) & 1023;
			}
			LOGMASKED(LOG_WRITE, "%s: %02x: drawing area bottom right %d,%d\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_drawarea_x2, n_drawarea_y2 );
			break;
		case 0xe5:
			n_drawoffset_x = util::sext( m_packet.n_entry[ 0 ] & 2047, 11 );
			if( m_n_gputype == 2 )
			{
				n_drawoffset_y = util::sext( ( m_packet.n_entry[ 0 ] >> 11 ) & 2047, 11 );
			}
			else
			{
				n_drawoffset_y = util::sext( ( m_packet.n_entry[ 0 ] >> 12 ) & 2047, 11 );
			}
			LOGMASKED(LOG_WRITE, "%s: %02x: drawing offset %d,%d\n", machine().describe_context(), m_packet.n_entry[ 0 ] >> 24,
				n_drawoffset_x, n_drawoffset_y );
			break;
		case 0xe6:
			m_draw_stp = BIT( data, 0 );
			m_check_stp = BIT( data, 1 );
			// TODO: confirm status bits on real type 1 gpu
			n_gpustatus &= ~( 3L << 0xb );
			n_gpustatus |= ( data & 0x03 ) << 0xb;
			LOGMASKED(LOG_WRITE, "%s: mask setting %d\n", machine().describe_context(), m_packet.n_entry[ 0 ] & 3);
			break;
		default:
#if defined( MAME_DEBUG )
			popmessage( "unknown GPU packet %08x", m_packet.n_entry[ 0 ] );
#endif
			logerror("%s: unknown GPU packet %08x (%08x)\n", machine().describe_context(), m_packet.n_entry[ 0 ], data);
#if ( STOP_ON_ERROR )
			n_gpu_buffer_offset = 1;
#endif
			break;
		}
		p_ram++;
		n_size--;
	}
}

void psxgpu_device::write(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	switch( offset )
	{
	case 0x00:
		gpu_write( &data, 1 );
		break;
	case 0x01:
		switch( data >> 24 )
		{
		case 0x00:
			LOGMASKED(LOG_WRITE, "%s: reset gpu\n", machine().describe_context());
			gpu_reset();
			break;
		case 0x01:
			LOGMASKED(LOG_WRITE, "%s: not handled: reset command buffer\n", machine().describe_context());
			n_gpu_buffer_offset = 0;
			break;
		case 0x02:
			LOGMASKED(LOG_WRITE, "%s: not handled: reset irq\n", machine().describe_context());
			break;
		case 0x03:
			n_gpustatus &= ~( 1L << 0x17 );
			n_gpustatus |= ( data & 0x01 ) << 0x17;
			break;
		case 0x04:
			LOGMASKED(LOG_WRITE, "%s: dma setup %d\n", machine().describe_context(), data & 3);
			n_gpustatus &= ~( 3L << 0x1d );
			n_gpustatus |= ( data & 0x03 ) << 0x1d;
			n_gpustatus &= ~( 1L << 0x19 );
			if( ( data & 3 ) == 1 || ( data & 3 ) == 2 )
			{
				n_gpustatus |= ( 1L << 0x19 );
			}
			break;
		case 0x05:
			m_n_displaystartx = data & 1023;
			if( m_n_gputype == 2 )
			{
				n_displaystarty = ( data >> 10 ) & 1023;
			}
			else
			{
				n_displaystarty = ( data >> 12 ) & 1023;
			}
			LOGMASKED(LOG_WRITE, "%s: start of display area %d %d\n", machine().describe_context(), m_n_displaystartx, n_displaystarty);
			if( m_hk_keep && ( m_n_displaystartx | n_displaystarty << 16 ) != m_hk_display ) // HIRAKU: in-between pictures
			{
				m_hk_display = m_n_displaystartx | n_displaystarty << 16;
				if( !m_hk_drawing.empty() )
					hk_new_frame( m_hk_vblanks );
				m_hk_no_flip = 0;
			}
			break;
		case 0x06:
			n_horiz_disstart = data & 4095;
			n_horiz_disend = ( data >> 12 ) & 4095;
			LOGMASKED(LOG_WRITE, "%s: horizontal display range %d %d\n", machine().describe_context(), n_horiz_disstart, n_horiz_disend);
			break;
		case 0x07:
			n_vert_disstart = data & 1023;
			n_vert_disend = ( data >> 10 ) & 2047;
			LOGMASKED(LOG_WRITE, "%s: vertical display range %d %d\n", machine().describe_context(), n_vert_disstart, n_vert_disend);
			break;
		case 0x08:
			LOGMASKED(LOG_WRITE, "%s: display mode %02x\n", machine().describe_context(), data & 0xff);
			n_gpustatus &= ~( 127L << 0x10 );
			n_gpustatus |= ( data & 0x3f ) << 0x11; /* width 0 + height + videmode + isrgb24 + isinter */
			n_gpustatus |= ( ( data & 0x40 ) >> 0x06 ) << 0x10; /* width 1 */
			if( m_n_gputype == 1 )
			{
				b_reverseflag = ( data >> 7 ) & 1;
			}
			updatevisiblearea();
			break;
		case 0x09:
			LOGMASKED(LOG_WRITE, "%s: not handled: GPU Control 0x09: %08x\n", machine().describe_context(), data);
			break;
		case 0x0d:
			LOGMASKED(LOG_WRITE, "%s: reset lightgun coordinates %08x\n", machine().describe_context(), data);
			n_lightgun_x = 0;
			n_lightgun_y = 0;
			break;
		case 0x10:
			switch( data & 0xff )
			{
			case 0x03:
				if( m_n_gputype == 2 )
				{
					n_gpuinfo = n_drawarea_x1 | ( n_drawarea_y1 << 10 );
				}
				else
				{
					n_gpuinfo = n_drawarea_x1 | ( n_drawarea_y1 << 12 );
				}
				LOGMASKED(LOG_WRITE, "%s: GPU Info - Draw area top left %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x04:
				if( m_n_gputype == 2 )
				{
					n_gpuinfo = n_drawarea_x2 | ( n_drawarea_y2 << 10 );
				}
				else
				{
					n_gpuinfo = n_drawarea_x2 | ( n_drawarea_y2 << 12 );
				}
				LOGMASKED(LOG_WRITE, "%s: GPU Info - Draw area bottom right %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x05:
				if( m_n_gputype == 2 )
				{
					n_gpuinfo = ( n_drawoffset_x & 2047 ) | ( ( n_drawoffset_y & 2047 ) << 11 );
				}
				else
				{
					n_gpuinfo = ( n_drawoffset_x & 2047 ) | ( ( n_drawoffset_y & 2047 ) << 12 );
				}
				LOGMASKED(LOG_WRITE, "%s: GPU Info - Draw offset %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x07:
				n_gpuinfo = m_n_gputype;
				LOGMASKED(LOG_WRITE, "%s: GPU Info - GPU Type %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			case 0x08:
				n_gpuinfo = n_lightgun_x | ( n_lightgun_y << 16 );
				LOGMASKED(LOG_WRITE, "%s: GPU Info - lightgun coordinates %08x\n", machine().describe_context(), n_gpuinfo);
				break;
			default:
				logerror("%s: GPU Info - unknown request (%08x)\n", machine().describe_context(), data);
				n_gpuinfo = 0;
				break;
			}
			break;
		case 0x20:
			LOGMASKED(LOG_WRITE, "%s: not handled: GPU Control 0x20: %08x\n", machine().describe_context(), data);
			break;
		default:
#if defined( MAME_DEBUG )
			popmessage( "unknown GPU command %08x", data );
#endif
			logerror("%s: gpu_w( %08x ) unknown GPU command\n", machine().describe_context(), data);
			break;
		}
		break;
	default:
		logerror("%s: gpu_w( %08x, %08x, %08x ) unknown register\n", machine().describe_context(), offset, data, mem_mask);
		break;
	}
}


void psxgpu_device::dma_read( uint32_t *p_n_psxram, uint32_t n_address, int32_t n_size )
{
	gpu_read( &p_n_psxram[ n_address / 4 ], n_size );
}

void psxgpu_device::gpu_read( uint32_t *p_ram, int32_t n_size )
{
	if( ( n_gpustatus & ( 1L << 0x1b ) ) != 0 )
		hk_sync(); // HIRAKU: VRAM to the CPU
	while( n_size > 0 )
	{
		if( ( n_gpustatus & ( 1L << 0x1b ) ) != 0 )
		{
			PAIR data;

			LOGMASKED(LOG_READ, "%s: copy image from frame buffer ( %d, %d )\n", machine().describe_context(), n_vramx, n_vramy);
			data.d = 0;
			for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
			{
				data.w.l = data.w.h;
				data.w.h = *( p_p_vram[ ( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 0x3ff ] + ( ( n_vramx + ( m_packet.n_entry[ 1 ] & 0xffff ) ) & 0x3ff ) );
				n_vramx++;
				if( n_vramx >= ( m_packet.n_entry[ 2 ] & 0xffff ) )
				{
					n_vramx = 0;
					n_vramy++;
					if( n_vramy >= ( m_packet.n_entry[ 2 ] >> 16 ) )
					{
						LOGMASKED(LOG_READ, "%s: copy image from frame buffer end\n", machine().describe_context());
						n_gpustatus &= ~( 1L << 0x1b );
						n_gpu_buffer_offset = 0;
						n_vramx = 0;
						n_vramy = 0;
						if( n_pixel == 0 )
						{
							data.w.l = data.w.h;
							data.w.h = 0;
						}
						break;
					}
				}
			}
			*( p_ram ) = data.d;
		}
		else
		{
			LOGMASKED(LOG_READ, "%s: read GPU info (%08x)\n", machine().describe_context(), n_gpuinfo);
			*( p_ram ) = n_gpuinfo;
		}
		p_ram++;
		n_size--;
	}
}

uint32_t psxgpu_device::read(offs_t offset, uint32_t mem_mask)
{
	uint32_t data;

	switch( offset )
	{
	case 0x00:
		gpu_read( &data, 1 );
		break;
	case 0x01:
		data = n_gpustatus;

		if ((((n_gpustatus & (1U << 22)) && (n_gpustatus & (1U << 13))) ||
			(!(n_gpustatus & (1U << 22)) && (BIT(screen().vpos(), 0)))))
			data |= 1U << 31;

		LOGMASKED(LOG_READ, "%s: read GPU status (%08x)\n", machine().describe_context(), data);
		break;
	default:
		logerror("%s: gpu_r( %08x, %08x ) unknown register\n", machine().describe_context(), offset, mem_mask);
		data = 0;
		break;
	}
	return data;
}

void psxgpu_device::vblank(screen_device &screen, bool vblank_state)
{
	if( vblank_state )
	{
#if PSXGPU_DEBUG_VIEWER
		DebugCheckKeys();
#endif

		if (n_gpustatus & (1U << 22))
			n_gpustatus ^= 1U << 13;
		else
			n_gpustatus |= 1U << 13;

		if( m_hk_keep ) // HIRAKU: in-between pictures
		{
			m_hk_vblanks++;
			if( ++m_hk_no_flip > 16 && !m_hk_drawing.empty() ) // a game that doesn't flip: a frame a VBLANK
				hk_new_frame( 1 );
		}
		m_vblank_handler(1);
	}
}

void psxgpu_device::gpu_reset()
{
	n_gpu_buffer_offset = 0;
	n_gpustatus = 0x14802000;
	n_drawarea_x1 = 0;
	n_drawarea_y1 = 0;
	n_drawarea_x2 = 1023;
	n_drawarea_y2 = 1023;
	n_drawoffset_x = 0;
	n_drawoffset_y = 0;
	m_n_displaystartx = 0;
	n_displaystarty = 0;
	n_horiz_disstart = 0x260;
	n_horiz_disend = 0xc60;
	n_vert_disstart = 0x010;
	n_vert_disend = 0x100;
	n_vramx = 0;
	n_vramy = 0;
	n_twx = 0;
	n_twy = 0;
	n_twh = 255;
	n_tww = 255;
	m_draw_stp = false;
	m_check_stp = false;
	updatevisiblearea();
}

void psxgpu_device::lightgun_set( int n_x, int n_y )
{
	n_lightgun_x = n_x;
	n_lightgun_y = n_y;
}

//-------------------------------------------------
//  device_config_complete - perform any
//  operations now that the configuration is
//  complete
//-------------------------------------------------

void psxgpu_device::device_config_complete()
{
	if (!has_screen())
		return;

	if (!screen().has_been_setup())
	{
		screen().set_refresh_hz(60);
		screen().set_vblank_time(ATTOSECONDS_IN_USEC(2500) /* not accurate */);
		screen().set_size(1024, 1024);
		screen().set_visarea(0, 639, 0, 479);
	}

	if (!screen().has_screen_update())
		screen().set_screen_update(*this, FUNC(psxgpu_device::update_screen));
}

// HIRAKU: the drawing thread (psx.h)

psxgpu_device::~psxgpu_device()
{
	device_stop();
}

void psxgpu_device::device_stop()
{
	if( m_hk_thread.joinable() )
	{
		hk_sync();
		m_hk_stop.store( true );
		m_hk_sleep.store( 0 );
		m_hk_sleep.notify_one();
		m_hk_thread.join();
	}
}

static inline void hk_pause()
{
#if defined( __x86_64__ ) || defined( __i386__ )
	__builtin_ia32_pause();
#elif defined( __aarch64__ )
	asm volatile( "yield" );
#endif
}

void psxgpu_device::hk_snapshot( draw_state &s ) const
{
	s.m_n_tx = m_n_tx; s.m_n_ty = m_n_ty; s.n_abr = n_abr; s.n_tp = n_tp; s.n_ix = n_ix; s.n_iy = n_iy; s.n_ti = n_ti;
	s.n_vramx = n_vramx; s.n_vramy = n_vramy; s.n_twy = n_twy; s.n_twx = n_twx; s.n_twh = n_twh; s.n_tww = n_tww;
	s.n_drawarea_x1 = n_drawarea_x1; s.n_drawarea_y1 = n_drawarea_y1; s.n_drawarea_x2 = n_drawarea_x2; s.n_drawarea_y2 = n_drawarea_y2;
	s.n_drawoffset_x = n_drawoffset_x; s.n_drawoffset_y = n_drawoffset_y;
	s.m_n_gputype = m_n_gputype; s.n_gpustatus = n_gpustatus;
	s.m_draw_stp = m_draw_stp; s.m_check_stp = m_check_stp;
	s.m_packet = m_packet;
}

// n_words free and contiguous at m_hk_write (an item never wraps: a skip item fills the ring's end)
uint32_t *psxgpu_device::hk_reserve( uint32_t n_words )
{
	uint32_t pos = m_hk_write & ( HK_RING - 1 );
	uint32_t need = n_words;
	if( pos + n_words > HK_RING )
		need += HK_RING - pos;
	uint64_t tail = m_hk_tail.load( std::memory_order_acquire );
	if( m_hk_write + need - tail > HK_RING )
	{
		if( !m_hk_thread.joinable() )
			hk_run();
		else
		{
			// full: wait for the worker
			for( int spin = 0; m_hk_write + need - ( tail = m_hk_tail.load( std::memory_order_acquire ) ) > HK_RING; spin++ )
			{
				if( spin < 4096 )
					hk_pause();
				else
				{
					m_hk_wait.store( 1 );
					if( m_hk_write + need - m_hk_tail.load() > HK_RING )
						m_hk_wait.wait( 1 );
					m_hk_wait.store( 0 );
				}
			}
		}
	}
	if( pos + n_words > HK_RING )
	{
		m_hk_ring[ pos ] = HK_SKIP;
		m_hk_ring[ pos + 1 ] = HK_RING - pos;
		m_hk_write += HK_RING - pos;
		pos = 0;
	}
	return &m_hk_ring[ pos ];
}

void psxgpu_device::hk_publish()
{
	uint32_t *item = m_hk_image != nullptr ? m_hk_image : &m_hk_ring[ m_hk_write & ( HK_RING - 1 ) ];
	if( m_hk_keep ) // in-between pictures: the frame's items
		m_hk_drawing.insert( m_hk_drawing.end(), item, item + item[ 1 ] );
	m_hk_write += item[ 1 ];
	m_hk_image = nullptr;
	if( m_hk_thread.joinable() )
	{
		// the rectangle it may write (its run's, when the last item's was the same)
		const hk_area a = hk_item_area( item );
		if( a.w > 0 && a.h > 0 )
		{
			if( !m_hk_pending.empty() && m_hk_pending.back().second == a )
				m_hk_pending.back().first = m_hk_write;
			else
				m_hk_pending.emplace_back( m_hk_write, a );
		}
	}
	m_hk_head.store( m_hk_write, std::memory_order_seq_cst );
	if( !m_hk_thread.joinable() )
		hk_run();
	else if( m_hk_sleep.load( std::memory_order_seq_cst ) != 0 )
	{
		m_hk_sleep.store( 0 );
		m_hk_sleep.notify_one();
	}
}

void psxgpu_device::hk_draw( uint32_t op, int n_points )
{
	hk_close();
	uint32_t *item = hk_reserve( HK_HEADER );
	item[ 0 ] = op | ( n_points << 8 );
	item[ 1 ] = HK_HEADER;
	hk_snapshot( *reinterpret_cast<draw_state *>( &item[ 2 ] ) );
	hk_publish();
}

void psxgpu_device::hk_image( uint32_t data )
{
	// the item's words stay at m_hk_write until it is published: [1] counts them
	if( m_hk_image != nullptr && m_hk_image[ 1 ] < 0x4000 && ( m_hk_image - m_hk_ring.get() ) + m_hk_image[ 1 ] < HK_RING )
	{
		m_hk_image[ m_hk_image[ 1 ]++ ] = data;
		return;
	}
	hk_close();
	uint32_t *item = hk_reserve( HK_HEADER + 0x4000 );
	item[ 0 ] = HK_IMAGE;
	item[ 1 ] = HK_HEADER;
	hk_snapshot( *reinterpret_cast<draw_state *>( &item[ 2 ] ) );
	item[ item[ 1 ]++ ] = data;
	m_hk_image = item;
}

// the queue drawn: VRAM as MAME's would be
void psxgpu_device::hk_sync()
{
	hk_close();
	if( !m_hk_thread.joinable() )
		return;
	const uint64_t head = m_hk_write;
	for( int spin = 0; m_hk_tail.load( std::memory_order_acquire ) != head; spin++ )
	{
		if( spin < 4096 )
			hk_pause();
		else
		{
			m_hk_wait.store( 1 );
			if( m_hk_tail.load() != head )
				m_hk_wait.wait( 1 );
			m_hk_wait.store( 0 );
		}
	}
	m_hk_pending.clear();
}

// VRAM's columns x to x + w (not wrapping: a row read past column 1023 runs into the next) and
// rows y to y + h (wrapping), as they would be with the queue drawn: waits only if a queued item
// may still write there
void psxgpu_device::hk_sync_area( int32_t x, int32_t w, int32_t y, int32_t h )
{
	if( !m_hk_thread.joinable() || w <= 0 || h <= 0 )
		return;
	hk_close();
	if( x < 0 || x + w > 1024 || m_hk_rows != 1024 )
	{
		hk_sync();
		return;
	}
	const uint64_t tail = m_hk_tail.load( std::memory_order_acquire );
	while( !m_hk_pending.empty() && m_hk_pending.front().first <= tail )
		m_hk_pending.pop_front();
	y &= 1023;
	h = std::min( h, 1024 );
	// two stretches of a circle of 1024 meet when either starts inside the other
	auto meet = []( int32_t a, int32_t la, int32_t b, int32_t lb ) { return ( ( b - a ) & 1023 ) < la || ( ( a - b ) & 1023 ) < lb; };
	for( const auto &[ end, a ] : m_hk_pending )
		if( meet( a.x, a.w, x, w ) && meet( a.y, a.h, y, h ) )
		{
			hk_sync();
			return;
		}
}

// draw what is published
void psxgpu_device::hk_run()
{
	uint64_t tail = m_hk_tail.load( std::memory_order_relaxed );
	const uint64_t head = m_hk_head.load( std::memory_order_acquire );
	raster &r = m_hk_raster;
	while( tail != head )
	{
		const uint32_t *item = &m_hk_ring[ tail & ( HK_RING - 1 ) ];
		const uint32_t op = item[ 0 ] & 0xff;
		if( op != HK_SKIP )
		{
			if( m_hk_undo_on.load( std::memory_order_relaxed ) && ( op == HK_IMAGE || op == HK_MoveImage || op == HK_FrameBufferRectangleDraw ) )
				hk_keep_undo( item, tail ); // in-between pictures
			hk_dispatch( r, item );
		}
		tail += item[ 1 ];
		m_hk_tail.store( tail, std::memory_order_seq_cst );
		if( m_hk_wait.load( std::memory_order_seq_cst ) != 0 )
		{
			m_hk_wait.store( 0 );
			m_hk_wait.notify_one();
		}
	}
}

void psxgpu_device::hk_worker()
{
	for( int spin = 0; !m_hk_stop.load( std::memory_order_relaxed ); )
	{
		if( m_hk_head.load( std::memory_order_acquire ) != m_hk_tail.load( std::memory_order_relaxed ) )
		{
			hk_run();
			spin = 0;
		}
		else if( spin++ < 256 )
			hk_pause(); // a moment for the next item (a longer spin bought little, for much more CPU)
		else
		{
			// idle: sleep until hk_publish() rings
			m_hk_sleep.store( 1 );
			if( m_hk_head.load() == m_hk_tail.load( std::memory_order_relaxed ) && !m_hk_stop.load() )
				m_hk_sleep.wait( 1 );
			m_hk_sleep.store( 0 );
			spin = 0;
		}
	}
}

// the CPU-to-VRAM transfer's words (gpu_write's loop, writing)
void psxgpu_device::raster::image( const uint32_t *p_data, uint32_t n_words )
{
	while( n_words-- > 0 )
	{
		uint32_t data = *( p_data++ );
		for( int n_pixel = 0; n_pixel < 2; n_pixel++ )
		{
			uint16_t *p_vram = p_p_vram[ ( n_vramy + ( m_packet.n_entry[ 1 ] >> 16 ) ) & 1023 ] + ( ( n_vramx + m_packet.n_entry[ 1 ] ) & 1023 );
			WRITE_PIXEL( data & 0xffff )
			n_vramx++;
			if( n_vramx >= ( m_packet.n_entry[ 2 ] & 0xffff ) )
			{
				n_vramx = 0;
				n_vramy++;
				if( n_vramy >= ( m_packet.n_entry[ 2 ] >> 16 ) )
				{
					n_vramx = 0;
					n_vramy = 0;
					break;
				}
			}
			data >>= 16;
		}
	}
}

// HIRAKU: in-between pictures (psx.h)

// an item drawn by r, the state it was queued with first
void psxgpu_device::hk_dispatch( raster &r, const uint32_t *item )
{
	const uint32_t op = item[ 0 ] & 0xff;
	const int n_points = item[ 0 ] >> 8;
	static_cast<draw_state &>( r ) = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
	switch( op )
	{
	case HK_FlatPolygon: r.FlatPolygon( n_points ); break;
	case HK_FlatTexturedPolygon: r.FlatTexturedPolygon( n_points ); break;
	case HK_GouraudPolygon: r.GouraudPolygon( n_points ); break;
	case HK_GouraudTexturedPolygon: r.GouraudTexturedPolygon( n_points ); break;
	case HK_MonochromeLine: r.MonochromeLine(); break;
	case HK_GouraudLine: r.GouraudLine(); break;
	case HK_FrameBufferRectangleDraw: r.FrameBufferRectangleDraw(); break;
	case HK_FlatRectangle: r.FlatRectangle(); break;
	case HK_FlatRectangle8x8: r.FlatRectangle8x8(); break;
	case HK_FlatRectangle16x16: r.FlatRectangle16x16(); break;
	case HK_FlatTexturedRectangle: r.FlatTexturedRectangle(); break;
	case HK_Sprite8x8: r.Sprite8x8(); break;
	case HK_Sprite16x16: r.Sprite16x16(); break;
	case HK_Dot: r.Dot(); break;
	case HK_TexturedDot: r.TexturedDot(); break;
	case HK_MoveImage: r.MoveImage(); break;
	case HK_IMAGE: r.image( &item[ HK_HEADER ], item[ 1 ] - HK_HEADER ); break;
	}
}

// the rectangle of VRAM an item may write: columns and rows modulo 1024, with a length
psxgpu_device::hk_area psxgpu_device::hk_item_area( const uint32_t *item ) const
{
	const draw_state &d = *reinterpret_cast<const draw_state *>( &item[ 2 ] );
	const PACKET &p = d.m_packet;
	switch( item[ 0 ] & 0xff )
	{
	case HK_FrameBufferRectangleDraw: // rows and columns wrap
		return { COORD_X( p.FlatRectangle.n_coord ) & 1023, std::min<int32_t>( SIZE_W( p.FlatRectangle.n_size ), 1024 ),
			COORD_Y( p.FlatRectangle.n_coord ) & 1023, std::min<int32_t>( SIZE_H( p.FlatRectangle.n_size ), 1024 ) };
	case HK_MoveImage: // to vertex 1, wrapping; its sizes are int16_t there
		return { COORD_X( p.MoveImage.vertex[ 1 ].n_coord ) & 1023, std::min<int32_t>( (int16_t)SIZE_W( p.MoveImage.n_size ), 1024 ),
			COORD_Y( p.MoveImage.vertex[ 1 ].n_coord ) & 1023, std::min<int32_t>( (int16_t)SIZE_H( p.MoveImage.n_size ), 1024 ) };
	case HK_IMAGE: // from n_vramx/y within the transfer's rectangle, wrapping; a size of 0 writes one pixel
		return { int32_t( p.n_entry[ 1 ] & 1023 ), std::clamp<int32_t>( p.n_entry[ 2 ] & 0xffff, 1, 1024 ),
			int32_t( ( p.n_entry[ 1 ] >> 16 ) & 1023 ), std::clamp<int32_t>( p.n_entry[ 2 ] >> 16, 1, 1024 ) };
	case HK_FlatRectangle16x16: // MAME tests the row before the drawing offset: any row
		return { int32_t( d.n_drawarea_x1 ), int32_t( d.n_drawarea_x2 ) - int32_t( d.n_drawarea_x1 ) + 1, 0, 1024 };
	default: // the primitives clip to the drawing area
		return { int32_t( d.n_drawarea_x1 ), int32_t( d.n_drawarea_x2 ) - int32_t( d.n_drawarea_x1 ) + 1,
			int32_t( d.n_drawarea_y1 ), int32_t( d.n_drawarea_y2 ) - int32_t( d.n_drawarea_y1 ) + 1 };
	}
}

// the items since the last frame are the frame on show now
void psxgpu_device::hk_new_frame( uint32_t vblanks )
{
	m_hk_shown_before = std::move( m_hk_shown );
	m_hk_shown = std::move( m_hk_drawing );
	m_hk_drawing.clear();
	m_hk_frame_vblanks = std::clamp<uint32_t>( vblanks, 1, 4 );
	m_hk_vblanks = 0;
	m_hk_frames++;
	m_hk_frame_at.store( m_hk_write, std::memory_order_release );
}

// (the drawing thread) before an upload: what it writes over
void psxgpu_device::hk_keep_undo( const uint32_t *item, uint64_t at )
{
	const uint64_t from = m_hk_frame_at.load( std::memory_order_acquire );
	while( !m_hk_undo.empty() && m_hk_undo.front().at < from )
		m_hk_undo.pop_front();
	hk_undo u{ at, hk_item_area( item ), {} };
	if( u.a.w > 0 && u.a.h > 0 )
	{
		u.pixels.resize( size_t( u.a.w ) * u.a.h );
		hk_area_copy( m_hk_raster.p_p_vram, u.a, u.pixels.data(), false );
	}
	m_hk_undo.push_back( std::move( u ) );
}

// An area's pixels out of VRAM (into: back into it), in the order the per-pixel loops took them
// (rows, then columns, both wrapping at 1024), a stretch of a row at a time: a frame's clear is
// 640x240 pixels, kept by the drawing thread and swapped twice by every in-between picture.
void psxgpu_device::hk_area_copy( uint16_t **p_p_vram, const hk_area &a, uint16_t *pixels, bool into )
{
	for( int32_t y = 0; y < a.h; y++ )
	{
		uint16_t *row = p_p_vram[ ( a.y + y ) & 1023 ];
		for( int32_t x = 0; x < a.w; )
		{
			const int32_t c = ( a.x + x ) & 1023, n = std::min( a.w - x, 1024 - c );
			if( into )
				std::copy_n( pixels, n, row + c );
			else
				std::copy_n( row + c, n, pixels );
			pixels += n;
			x += n;
		}
	}
}

bool psxgpu_device::hk_between( double t, screen_device &screen, bitmap_rgb32 &bitmap )
{
	if( !m_hk_keep )
	{
		m_hk_keep = true; // from now on
		m_hk_drawing.clear();
		m_hk_frame_at.store( m_hk_write );
		m_hk_undo_on.store( true );
		return false;
	}
	if( m_hk_shown.empty() || m_hk_shown_before.empty() )
		return false;
	hk_sync(); // VRAM as the queue leaves it, the worker idle
	// how far on, in the frame's moves: a frame shown for two VBLANKs moves half its step a VBLANK
	const double on = std::min( ( m_hk_vblanks + t ) / m_hk_frame_vblanks, 1.5 );
	// where an item's vertices are in its packet (n_entry), and how many: none for what doesn't move
	auto vertices = []( uint32_t op, int n_points, int *at ) {
		int n = 0, step = 1;
		switch( op )
		{
		case HK_FlatPolygon: n = n_points; break;
		case HK_FlatTexturedPolygon: case HK_GouraudPolygon: n = n_points, step = 2; break;
		case HK_GouraudTexturedPolygon: n = n_points, step = 3; break;
		case HK_MonochromeLine: n = 2; break;
		case HK_GouraudLine: n = 2, step = 2; break;
		case HK_FlatRectangle: case HK_FlatRectangle8x8: case HK_FlatRectangle16x16: case HK_FlatTexturedRectangle:
		case HK_Sprite8x8: case HK_Sprite16x16: case HK_Dot: case HK_TexturedDot: n = 1; break;
		}
		for( int i = 0; i < n; i++ )
			at[ i ] = 1 + i * step;
		return n;
	};

	// Each primitive drawn is matched with one of the frame before's (its first vertex within
	// ScrollGuess::REACH pixels) with the same command: a rectangle or sprite with the same colour and
	// at most one other word of its packet different (its size: a note clipped shorter going off the
	// lane; its texture: an animated one); a polygon or line with the same texture (a shaded one's
	// colours change with its light). The scroll is voted first (hiraku/scroll_guess.h, pair: what
	// is fast may be further than 16 pixels on).
	std::vector<uint32_t> items = m_hk_shown;
	bool moved = false;
	// once a frame: its other refreshes' pictures share the pairing (a screen of identical tiles
	// between scenes made it 18 ms, and at 240 Hz a frame has four or five pictures)
	if( m_hk_moves_frame != m_hk_frames || m_hk_moves_size != items.size() )
	{
		m_hk_moves_frame = m_hk_frames;
		m_hk_moves_size = items.size();
		m_hk_moves.clear();
		std::vector<const uint32_t *> before;
		for( size_t i = 0; i < m_hk_shown_before.size(); i += m_hk_shown_before[ i + 1 ] )
			before.push_back( &m_hk_shown_before[ i ] );
		auto packet = []( const uint32_t *item ) { return reinterpret_cast<const draw_state *>( &item[ 2 ] )->m_packet.n_entry; };
		// the frame before's primitives by their command, colour and place (REACH-sized cells): only
		// those can match (a screen of 943 identical tiles took 10 ms compared every one with every one)
		auto look = [&packet]( const uint32_t *it, int cx, int cy ) {
			const uint32_t op = it[ 0 ] & 0xff, w = packet( it )[ 0 ];
			const bool g = op == HK_GouraudPolygon || op == HK_GouraudTexturedPolygon || op == HK_GouraudLine;
			const uint64_t l = uint64_t( it[ 0 ] ) << 32 | ( g ? w >> 24 : w );
			return l * 0x9e3779b97f4a7c15ull ^ ( uint64_t( uint16_t( cx ) ) << 16 | uint16_t( cy ) );
		};
		auto cell = [&packet, &vertices]( const uint32_t *it, int &cx, int &cy ) {
			int at[ 4 ];
			if( vertices( it[ 0 ] & 0xff, it[ 0 ] >> 8, at ) == 0 )
				return false;
			const uint32_t v = packet( it )[ at[ 0 ] ];
			cx = int( int16_t( v & 0xffff ) ) >> 6, cy = int( int16_t( v >> 16 ) ) >> 6; // floor: REACH is 64
			return true;
		};
		static_assert( hk::ScrollGuess::REACH == 64 );
		std::unordered_map<uint64_t, std::vector<int>> alike;
		for( size_t kb = 0; kb < before.size(); kb++ )
			if( int cx, cy; cell( before[ kb ], cx, cy ) )
				alike[ look( before[ kb ], cx, cy ) ].push_back( (int)kb );
		std::vector<size_t> objects; // each candidate's object: its item
		std::vector<int> number; // ... and its number among the items (as the frame before's are in before)
		int items_n = 0;
		std::vector<hk::ScrollGuess::Candidate> cands;
		for( size_t i = 0; i < items.size(); i += items[ i + 1 ], items_n++ )
		{
			uint32_t *item = &items[ i ];
			int at[ 4 ];
			const int n = vertices( item[ 0 ] & 0xff, item[ 0 ] >> 8, at );
			if( n == 0 )
				continue;
			uint32_t *c = reinterpret_cast<draw_state *>( &item[ 2 ] )->m_packet.n_entry;
			if( n > 1 )
			{
				// a polygon only if it's a rectangle on the screen's axes (a sprite drawn as a quad: an
				// arrow, a note), a line only if it's level or upright (a bar line). A 3D model's
				// polygons, each moving its own way, would come apart at their seams
				int xs[ 4 ], ys[ 4 ], nx = 0, ny = 0;
				for( int v = 0; v < n; v++ )
				{
					const int px = (int16_t)( c[ at[ v ] ] & 0xffff ), py = (int16_t)( c[ at[ v ] ] >> 16 );
					if( std::find( xs, xs + nx, px ) == xs + nx ) xs[ nx++ ] = px;
					if( std::find( ys, ys + ny, py ) == ys + ny ) ys[ ny++ ] = py;
				}
				if( n == 4 ? nx > 2 || ny > 2 : n == 2 ? nx > 1 && ny > 1 : true )
					continue;
			}
			const int words = at[ n - 1 ] + 4 > 12 ? 12 : at[ n - 1 ] + 4; // the packet's words, and a few after
			const int x = (int16_t)( c[ at[ 0 ] ] & 0xffff ), y = (int16_t)( c[ at[ 0 ] ] >> 16 );
			const uint32_t op = item[ 0 ] & 0xff;
			const bool gouraud = op == HK_GouraudPolygon || op == HK_GouraudTexturedPolygon || op == HK_GouraudLine;
			const bool textured = op == HK_FlatTexturedPolygon || op == HK_GouraudTexturedPolygon;
			const int obj = (int)objects.size();
			objects.push_back( i );
			number.push_back( items_n );
			std::vector<int> near; // the frame before's like it in its cell and the eight around
			int cx, cy;
			cell( item, cx, cy );
			for( int ey = -1; ey <= 1; ey++ )
				for( int ex = -1; ex <= 1; ex++ )
					if( const auto same = alike.find( look( item, cx + ex, cy + ey ) ); same != alike.end() )
						near.insert( near.end(), same->second.begin(), same->second.end() );
			std::sort( near.begin(), near.end() ); // in the frame before's order, as compared before
			for( const int kbi : near )
			{
				const size_t kb = (size_t)kbi;
				const uint32_t *b = before[ kb ];
				if( b[ 0 ] != item[ 0 ] ) // the same primitive, with as many points
					continue;
				const uint32_t *p = packet( b );
				if( gouraud ? ( p[ 0 ] >> 24 ) != ( c[ 0 ] >> 24 ) : p[ 0 ] != c[ 0 ] )
					continue;
				int other = 0;
				if( n > 1 )
				{
					// polygons and lines: the same texture (each vertex's word after its coordinates);
					// a shaded one's colours change with its light as a model moves
					for( int v = 0; textured && v < n; v++ )
						other += p[ at[ v ] + 1 ] != c[ at[ v ] + 1 ] ? 2 : 0;
				}
				else
				{
					for( int w = 1, v = 0; w < words && other < 2; w++ )
					{
						if( v < n && w == at[ v ] )
						{
							v++;
							continue;
						}
						other += p[ w ] != c[ w ];
					}
				}
				if( other > 1 )
					continue;
				const int bx = (int16_t)( p[ at[ 0 ] ] & 0xffff ), by = (int16_t)( p[ at[ 0 ] ] >> 16 );
				if( std::abs( x - bx ) > hk::ScrollGuess::REACH || std::abs( y - by ) > hk::ScrollGuess::REACH )
					continue;
				// the one whose other vertices moved most as the first did (a model's polygons crowd each
				// other; the same polygon keeps its shape), a shaded one's colours closest; carried on
				// only if it moved whole, every vertex by the same step (within a pixel): what changes
				// shape (grows, turns) stays as the game drew it
				const int dx = x - bx, dy = y - by;
				int cost = 8 * other;
				bool whole = true;
				for( int v = 0; v < n; v++ )
				{
					const int ex = (int16_t)( c[ at[ v ] ] & 0xffff ) - (int16_t)( p[ at[ v ] ] & 0xffff ) - dx;
					const int ey = (int16_t)( c[ at[ v ] ] >> 16 ) - (int16_t)( p[ at[ v ] ] >> 16 ) - dy;
					cost += std::abs( ex ) + std::abs( ey );
					whole = whole && std::abs( ex ) <= 1 && std::abs( ey ) <= 1;
					for( int k = 0; gouraud && k < 24; k += 8 )
						cost += std::abs( int( ( c[ at[ v ] - 1 ] >> k ) & 0xff ) - int( ( p[ at[ v ] - 1 ] >> k ) & 0xff ) ) / 16;
				}
				cands.push_back( { obj, (int)kb, dx, dy, cost, whole } );
			}
		}
		const std::vector<int> paired = m_hk_scroll.pair( m_hk_frames, (int)objects.size(), cands );
		// each item's move and its number in the frame before (its own track), by its number
		std::vector<std::pair<int, int>> steps( items_n, { 0, 0 } );
		std::vector<int> was( items_n, -1 );
		for( size_t o = 0; o < objects.size(); o++ )
		{
			if( paired[ o ] < 0 )
				continue;
			const hk::ScrollGuess::Candidate &m = cands[ (size_t)paired[ o ] ];
			if( !m.whole )
				continue;
			was[ number[ o ] ] = m.from;
			steps[ number[ o ] ] = { m.dx, m.dy };
			if( m.dx != 0 || m.dy != 0 )
				m_hk_moves.push_back( { objects[ o ], number[ o ], m.dx, m.dy } );
		}
		// carried on: what scrolls with the frame's other moving items along the scroll's steady line
		// (hiraku/scroll_guess.h: notes step whole pixels, and carried on by their last step they
		// judder), the rest along its own track's, else by its last move
		m_hk_scroll.frame( m_hk_frames, steps, was );
	}
	for( const auto &m : m_hk_moves )
	{
		int mx, my;
		m_hk_scroll.offset( m.dx, m.dy, on, mx, my, m.n );
		if( mx == 0 && my == 0 )
			continue;
		moved = true;
		uint32_t *item = &items[ m.i ];
		int at[ 4 ];
		const int n = vertices( item[ 0 ] & 0xff, item[ 0 ] >> 8, at );
		uint32_t *c = reinterpret_cast<draw_state *>( &item[ 2 ] )->m_packet.n_entry;
		for( int v = 0; v < n; v++ )
		{
			const uint32_t w = c[ at[ v ] ];
			c[ at[ v ] ] = ( uint32_t( uint16_t( int16_t( w >> 16 ) + my ) ) << 16 ) | uint16_t( int16_t( w & 0xffff ) + mx );
		}
	}

	if( !moved )
		return false; // nothing on by a whole pixel yet: the frame as it is
	// the uploads since the frame on show undone, newest first (and done again at the end)
	std::vector<std::pair<hk_area, std::vector<uint16_t>>> redo;
	const uint64_t from = m_hk_frame_at.load();
	for( auto u = m_hk_undo.rbegin(); u != m_hk_undo.rend() && u->at >= from; ++u )
	{
		std::vector<uint16_t> now;
		if( u->a.w > 0 && u->a.h > 0 && u->a.h <= m_hk_rows )
		{
			// no pixel twice (the area is at most 1024 columns, and no taller than VRAM): its
			// pixels as they are kept, then the kept ones put back, as the loop below does one by one
			now.resize( u->pixels.size() );
			hk_area_copy( p_p_vram, u->a, now.data(), false );
			hk_area_copy( p_p_vram, u->a, const_cast<uint16_t *>( u->pixels.data() ), true );
		}
		else
		{
			now.reserve( u->pixels.size() );
			size_t k = 0;
			for( int32_t y = 0; y < u->a.h; y++ )
				for( int32_t x = 0; x < u->a.w; x++ )
				{
					uint16_t &px = p_p_vram[ ( u->a.y + y ) & 1023 ][ ( u->a.x + x ) & 1023 ];
					now.push_back( px );
					px = u->pixels[ k++ ];
				}
		}
		redo.emplace_back( u->a, std::move( now ) );
	}

	// the rows the frame draws on kept as they are, the frame drawn again, composed, and the rows
	// put back
	std::vector<uint8_t> kept( 1024, 0 );
	std::vector<uint32_t> rows;
	std::vector<uint16_t> pixels;
	for( size_t i = 0; i < items.size(); i += items[ i + 1 ] )
	{
		const hk_area a = hk_item_area( &items[ i ] );
		for( int32_t r = 0; r < std::min<int32_t>( a.h, 1024 ); r++ )
		{
			const uint32_t y = uint32_t( a.y + r ) & 1023;
			if( kept[ y ] )
				continue;
			kept[ y ] = 1;
			rows.push_back( y );
			pixels.insert( pixels.end(), p_p_vram[ y ], p_p_vram[ y ] + 1024 );
		}
	}
	auto r = std::make_unique<raster>();
	std::copy_n( p_p_vram, 1024, r->p_p_vram );
	for( size_t i = 0; i < items.size(); i += items[ i + 1 ] )
		hk_dispatch( *r, &items[ i ] );
	bitmap.resize( screen.m_bitmap.width(), screen.m_bitmap.height() );
	update_screen( screen, bitmap, screen.visible_area() );
	for( size_t i = rows.size(); i-- > 0; ) // in reverse: rows beyond VRAM's height alias
		std::copy_n( &pixels[ i * 1024 ], 1024, p_p_vram[ rows[ i ] ] );
	for( auto r = redo.rbegin(); r != redo.rend(); ++r ) // oldest first
	{
		if( r->first.w > 0 && r->first.h > 0 )
			hk_area_copy( p_p_vram, r->first, r->second.data(), true );
	}
	return true;
}
