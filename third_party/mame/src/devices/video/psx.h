// license:BSD-3-Clause
// copyright-holders:smf
/*
 * PlayStation GPU emulator
 *
 * Copyright 2003-2014 smf
 *
 */

#ifndef MAME_VIDEO_PSX_H
#define MAME_VIDEO_PSX_H

#pragma once

#include "machine/ram.h"

#include <atomic>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hiraku/scroll_guess.h"

#define PSXGPU_DEBUG_VIEWER ( 0 )

DECLARE_DEVICE_TYPE(CXD8514Q,  cxd8514q_device)
DECLARE_DEVICE_TYPE(CXD8538Q,  cxd8538q_device)
DECLARE_DEVICE_TYPE(CXD8561Q,  cxd8561q_device)
DECLARE_DEVICE_TYPE(CXD8561BQ, cxd8561bq_device)
DECLARE_DEVICE_TYPE(CXD8561CQ, cxd8561cq_device)
DECLARE_DEVICE_TYPE(CXD8654Q,  cxd8654q_device)

class psxcpu_device;

class psxgpu_device : public device_t, public device_video_interface, public device_palette_interface
{
public:
	// configuration helpers
	void set_cpu(psxcpu_device* cpu);
	template<typename T> void set_ram(T &&tag) { m_ram.set_tag(std::forward<T>(tag)); }
	void set_vclkn(XTAL clock) { m_vclk[0] = clock.value(); } // pin 192 on SGRAM GPU
	void set_vclkp(XTAL clock) { m_vclk[1] = clock.value(); } // pin 196 on SGRAM GPU
	auto vblank_callback() { return m_vblank_handler.bind(); }

	void write(offs_t offset, uint32_t data, uint32_t mem_mask = ~0);
	uint32_t read(offs_t offset, uint32_t mem_mask = ~0);
	void dma_read( uint32_t *ram, uint32_t n_address, int32_t n_size );
	void dma_write( uint32_t *ram, uint32_t n_address, int32_t n_size );
	void lightgun_set( int, int );

	static constexpr feature_type imperfect_features() { return feature::GRAPHICS; }

	virtual ~psxgpu_device(); // HIRAKU: the drawing thread

protected:
	static constexpr unsigned MAX_LEVEL = 32;
	static constexpr unsigned MID_LEVEL = (MAX_LEVEL / 2) << 8;
	static constexpr unsigned MAX_SHADE = 0x100;
	static constexpr unsigned MID_SHADE = 0x80;

	// construction/destruction
	psxgpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock);

	virtual void device_start() override ATTR_COLD;
	virtual void device_post_load() override;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_stop() override; // HIRAKU
	virtual void device_config_complete() override;

	// device_palette_interface overrides
	virtual uint32_t palette_entries() const noexcept override { return 32*32*32*2; }

	required_device<ram_device> m_ram;

private:
	static constexpr unsigned DEBUG_COORDS = 10;

	struct psx_gpu_debug
	{
		std::unique_ptr<bitmap_ind16> mesh;
		int b_clear;
		int b_mesh;
		int n_skip;
		int b_texture;
		int n_interleave;
		int n_coord;
		int n_coordx[ DEBUG_COORDS ];
		int n_coordy[ DEBUG_COORDS ];
	};

	struct FLATVERTEX
	{
		PAIR n_coord;
	};

	struct GOURAUDVERTEX
	{
		PAIR n_bgr;
		PAIR n_coord;
	};

	struct FLATTEXTUREDVERTEX
	{
		PAIR n_coord;
		PAIR n_texture;
	};

	struct GOURAUDTEXTUREDVERTEX
	{
		PAIR n_bgr;
		PAIR n_coord;
		PAIR n_texture;
	};

	union PACKET
	{
		uint32_t n_entry[ 16 ];

		struct
		{
			PAIR n_cmd;
			struct FLATVERTEX vertex[ 2 ];
			PAIR n_size;
		} MoveImage;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_size;
		} FlatRectangle;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
		} FlatRectangle8x8;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
		} FlatRectangle16x16;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_texture;
		} Sprite8x8;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_texture;
		} Sprite16x16;

		struct
		{
			PAIR n_bgr;
			PAIR n_coord;
			PAIR n_texture;
			PAIR n_size;
		} FlatTexturedRectangle;

		struct
		{
			PAIR n_bgr;
			struct FLATVERTEX vertex[ 4 ];
		} FlatPolygon;

		struct
		{
			struct GOURAUDVERTEX vertex[ 4 ];
		} GouraudPolygon;

		struct
		{
			PAIR n_bgr;
			struct FLATVERTEX vertex[ 2 ];
		} MonochromeLine;

		struct
		{
			struct GOURAUDVERTEX vertex[ 2 ];
		} GouraudLine;

		struct
		{
			PAIR n_bgr;
			struct FLATTEXTUREDVERTEX vertex[ 4 ];
		} FlatTexturedPolygon;

		struct
		{
			struct GOURAUDTEXTUREDVERTEX vertex[ 4 ];
		} GouraudTexturedPolygon;

		struct
		{
			PAIR n_bgr;
			struct FLATVERTEX vertex;
		} Dot;

		struct
		{
			PAIR n_bgr;
			struct FLATTEXTUREDVERTEX vertex;
		} TexturedDot;
	};

	void updatevisiblearea();
	void decode_tpage( uint32_t tpage );

	// HIRAKU: the drawing on a worker thread (PLAN.md section 19). The command parser, the status
	// and every register stay here, on the emulation thread; what writes VRAM (the primitives,
	// the CPU-to-VRAM image, MoveImage) goes into a queue with a copy of the state the drawing
	// reads (draw_state), and raster draws it on the worker, in order. What reads VRAM waits for
	// the queued items that could change what it reads: VRAM-to-CPU for all of them; the screen
	// update for those whose rectangle meets the rows and columns it shows (games draw the next
	// picture into the other buffer). So every read sees what MAME's would.
	// The textured polygons' texture page reaches the status here as in MAME (decode_tpage()
	// beside the queueing), and again in raster's own copy of the state.
	// HK_GPU_THREAD=0: no worker; each item is drawn on the emulation thread as it is queued.
	struct draw_state
	{
		int32_t m_n_tx, m_n_ty, n_abr, n_tp, n_ix, n_iy, n_ti;
		uint32_t n_vramx, n_vramy, n_twy, n_twx, n_twh, n_tww;
		uint32_t n_drawarea_x1, n_drawarea_y1, n_drawarea_x2, n_drawarea_y2;
		int32_t n_drawoffset_x, n_drawoffset_y;
		int m_n_gputype;
		uint32_t n_gpustatus;
		bool m_draw_stp, m_check_stp;
		PACKET m_packet;
	};
	struct raster : draw_state
	{
		uint16_t *p_p_vram[ 1024 ];
		template <typename... T> void logerror( T &&... ) const { }
		void decode_tpage( uint32_t tpage );
		void FlatPolygon( int n_points );
		void FlatTexturedPolygon( int n_points );
		void GouraudPolygon( int n_points );
		void GouraudTexturedPolygon( int n_points );
		void MonochromeLine();
		void GouraudLine();
		void FrameBufferRectangleDraw();
		void FlatRectangle();
		void FlatRectangle8x8();
		void FlatRectangle16x16();
		void FlatTexturedRectangle();
		void Sprite8x8();
		void Sprite16x16();
		void Dot();
		void TexturedDot();
		void MoveImage();
		void image( const uint32_t *p_data, uint32_t n_words );
	};
	enum : uint32_t { HK_FlatPolygon, HK_FlatTexturedPolygon, HK_GouraudPolygon, HK_GouraudTexturedPolygon, HK_MonochromeLine, HK_GouraudLine, HK_FrameBufferRectangleDraw, HK_FlatRectangle, HK_FlatRectangle8x8, HK_FlatRectangle16x16, HK_FlatTexturedRectangle, HK_Sprite8x8, HK_Sprite16x16, HK_Dot, HK_TexturedDot, HK_MoveImage, HK_IMAGE, HK_SKIP };
	void FlatPolygon( int n_points ) { hk_draw( HK_FlatPolygon, n_points ); }
	void FlatTexturedPolygon( int n_points ) { hk_draw( HK_FlatTexturedPolygon, n_points ); decode_tpage( m_packet.FlatTexturedPolygon.vertex[ 1 ].n_texture.w.h ); }
	void GouraudPolygon( int n_points ) { hk_draw( HK_GouraudPolygon, n_points ); }
	void GouraudTexturedPolygon( int n_points ) { hk_draw( HK_GouraudTexturedPolygon, n_points ); decode_tpage( m_packet.GouraudTexturedPolygon.vertex[ 1 ].n_texture.w.h ); }
	void MonochromeLine() { hk_draw( HK_MonochromeLine, 0 ); }
	void GouraudLine() { hk_draw( HK_GouraudLine, 0 ); }
	void FrameBufferRectangleDraw() { hk_draw( HK_FrameBufferRectangleDraw, 0 ); }
	void FlatRectangle() { hk_draw( HK_FlatRectangle, 0 ); }
	void FlatRectangle8x8() { hk_draw( HK_FlatRectangle8x8, 0 ); }
	void FlatRectangle16x16() { hk_draw( HK_FlatRectangle16x16, 0 ); }
	void FlatTexturedRectangle() { hk_draw( HK_FlatTexturedRectangle, 0 ); }
	void Sprite8x8() { hk_draw( HK_Sprite8x8, 0 ); }
	void Sprite16x16() { hk_draw( HK_Sprite16x16, 0 ); }
	void Dot() { hk_draw( HK_Dot, 0 ); }
	void TexturedDot() { hk_draw( HK_TexturedDot, 0 ); }
	void MoveImage() { hk_draw( HK_MoveImage, 0 ); }
	void hk_snapshot( draw_state &s ) const;
	uint32_t *hk_reserve( uint32_t n_words );
	void hk_publish();
	void hk_draw( uint32_t op, int n_points );
	void hk_image( uint32_t data );
	void hk_close() { if( m_hk_image != nullptr ) hk_publish(); }
	void hk_sync();
	void hk_sync_area( int32_t x, int32_t w, int32_t y, int32_t h );
	void hk_run();
	void hk_worker();

	static constexpr uint32_t HK_RING = 1 << 20; // words
	static constexpr uint32_t HK_HEADER = 2 + ( sizeof( draw_state ) + 3 ) / 4;
	std::unique_ptr<uint32_t[]> m_hk_ring;
	std::atomic<uint64_t> m_hk_head{ 0 }; // published: the worker draws up to here
	std::atomic<uint64_t> m_hk_tail{ 0 }; // drawn
	std::atomic<uint32_t> m_hk_sleep{ 0 }, m_hk_wait{ 0 }; // the worker asleep; the emulation thread waiting
	std::atomic<bool> m_hk_stop{ false };
	uint64_t m_hk_write = 0; // the end of what is queued, published or not
	uint32_t *m_hk_image = nullptr; // the open image item, words still being added
	raster m_hk_raster;
	std::thread m_hk_thread;
	// what the queued items may still write: a rectangle of VRAM per run of items, with the queue
	// position where the run ends. Columns and rows are taken modulo 1024 (x, y) with a length.
	struct hk_area { int32_t x, w, y, h; bool operator==( const hk_area & ) const = default; };
	std::deque<std::pair<uint64_t, hk_area>> m_hk_pending;
	int32_t m_hk_rows = 0; // VRAM's height (rows beyond it alias)

	// HIRAKU: in-between pictures (Core::render_between, --smooth-motion). Once asked for, the
	// queued items are kept (each holds all it draws from), a frame's from one flip of the display
	// to the next (a write of a new display start: the game shows what it drew since its last
	// flip); a game that never flips (it draws where it shows) has a frame each VBLANK. hk_between()
	// draws the frame on show again, what moved whole carried on along its last move (matched with
	// the frame before's), `t` of a VBLANK on from this one's (a game at 30 frames a second moves half
	// a step a VBLANK), into VRAM, composes it into `bitmap` with update_screen(), and puts VRAM
	// back. false: nothing to draw from yet.
public:
	bool hk_between( double t, screen_device &screen, bitmap_rgb32 &bitmap );
private:
	void hk_dispatch( raster &r, const uint32_t *item );
	hk_area hk_item_area( const uint32_t *item ) const;
	void hk_new_frame( uint32_t vblanks );
	bool m_hk_keep = false;
	std::vector<uint32_t> m_hk_drawing, m_hk_shown, m_hk_shown_before; // items, back to back
	uint32_t m_hk_display = ~0u;    // the display start last written (x | y << 16)
	uint32_t m_hk_vblanks = 0;      // VBLANKs since the last frame began to show
	uint32_t m_hk_frame_vblanks = 1; // ... and how many the frame before it showed for
	uint32_t m_hk_no_flip = 1000;   // VBLANKs since the last flip
	uint64_t m_hk_frames = 0;       // frames shown (hk_new_frame): the scroll guess counts once a frame
	hk::ScrollGuess m_hk_scroll;    // what scrolls with the frame's other moving items: its steady line
	struct hk_move { size_t i; int n, dx, dy; };
	std::vector<hk_move> m_hk_moves; // the frame's pairing: what moved whole, and by how much (its item, its number among them)
	uint64_t m_hk_moves_frame = ~uint64_t( 0 ); // ... for this frame
	size_t m_hk_moves_size = 0;
	// what the uploads (CPU-to-VRAM, MoveImage, fills) wrote over, kept by the drawing thread from the
	// queue's position where the frame after the one on show begins: the textures and palettes that
	// frame changes (the game prepares it while this one shows) put back while this one is drawn again
	struct hk_undo { uint64_t at; hk_area a; std::vector<uint16_t> pixels; };
	void hk_keep_undo( const uint32_t *item, uint64_t at );
	static void hk_area_copy( uint16_t **p_p_vram, const hk_area &a, uint16_t *pixels, bool into );
	std::atomic<bool> m_hk_undo_on{ false };
	std::atomic<uint64_t> m_hk_frame_at{ 0 };
	std::deque<hk_undo> m_hk_undo;

	void psx_gpu_init( int n_gputype );
	void gpu_reset();
	void gpu_read( uint32_t *p_ram, int32_t n_size );
	void gpu_write( uint32_t *p_ram, int32_t n_size );

	int32_t m_n_tx;
	int32_t m_n_ty;
	int32_t n_abr;
	int32_t n_tp;
	int32_t n_ix;
	int32_t n_iy;
	int32_t n_ti;

	uint32_t n_vramx;
	uint32_t n_vramy;
	uint32_t n_twy;
	uint32_t n_twx;
	uint32_t n_twh;
	uint32_t n_tww;
	uint32_t n_drawarea_x1;
	uint32_t n_drawarea_y1;
	uint32_t n_drawarea_x2;
	uint32_t n_drawarea_y2;
	uint32_t n_horiz_disstart;
	uint32_t n_horiz_disend;
	uint32_t n_vert_disstart;
	uint32_t n_vert_disend;
	uint32_t b_reverseflag;
	int32_t n_drawoffset_x;
	int32_t n_drawoffset_y;
	uint32_t m_n_displaystartx;
	uint32_t n_displaystarty;
	int m_n_gputype;
	uint32_t n_gpustatus;
	uint32_t n_gpuinfo;
	uint32_t n_gpu_buffer_offset;
	uint32_t n_lightgun_x;
	uint32_t n_lightgun_y;
	uint32_t n_screenwidth;
	uint32_t n_screenheight;
	bool m_draw_stp;
	bool m_check_stp;

	PACKET m_packet;

	uint16_t *p_p_vram[ 1024 ];

	// HIRAKU: static, for raster
	static inline uint16_t p_n_redshade[ MAX_LEVEL * MAX_SHADE ];
	static inline uint16_t p_n_greenshade[ MAX_LEVEL * MAX_SHADE ];
	static inline uint16_t p_n_blueshade[ MAX_LEVEL * MAX_SHADE ];
	static inline uint16_t p_n_redlevel[ 0x10000 ];
	static inline uint16_t p_n_greenlevel[ 0x10000 ];
	static inline uint16_t p_n_bluelevel[ 0x10000 ];

	static inline uint16_t p_n_f025[ MAX_LEVEL * MAX_SHADE ];
	static inline uint16_t p_n_f05[ MAX_LEVEL * MAX_SHADE ];
	static inline uint16_t p_n_f1[ MAX_LEVEL * MAX_SHADE ];
	static inline uint16_t p_n_redb05[ 0x10000 ];
	static inline uint16_t p_n_greenb05[ 0x10000 ];
	static inline uint16_t p_n_blueb05[ 0x10000 ];
	static inline uint16_t p_n_redb1[ 0x10000 ];
	static inline uint16_t p_n_greenb1[ 0x10000 ];
	static inline uint16_t p_n_blueb1[ 0x10000 ];
	static inline uint16_t p_n_redaddtrans[ MAX_LEVEL * MAX_LEVEL ];
	static inline uint16_t p_n_greenaddtrans[ MAX_LEVEL * MAX_LEVEL ];
	static inline uint16_t p_n_blueaddtrans[ MAX_LEVEL * MAX_LEVEL ];
	static inline uint16_t p_n_redsubtrans[ MAX_LEVEL * MAX_LEVEL ];
	static inline uint16_t p_n_greensubtrans[ MAX_LEVEL * MAX_LEVEL ];
	static inline uint16_t p_n_bluesubtrans[ MAX_LEVEL * MAX_LEVEL ];

	static inline uint32_t p_n_g0r0[ 0x10000 ];
	static inline uint32_t p_n_b0[ 0x10000 ];
	static inline uint32_t p_n_r1[ 0x10000 ];
	static inline uint32_t p_n_b1g1[ 0x10000 ];

	devcb_write_line m_vblank_handler;
	uint32_t m_vclk[2];

	void vblank(screen_device &screen, bool vblank_state);
	uint32_t update_screen(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);

#if defined(PSXGPU_DEBUG_VIEWER) && PSXGPU_DEBUG_VIEWER
	void DebugMeshInit();
	void DebugMesh( int n_coordx, int n_coordy );
	void DebugMeshEnd();
	void DebugCheckKeys();
	int DebugMeshDisplay( bitmap_rgb32 &bitmap, const rectangle &cliprect );
	int DebugTextureDisplay( bitmap_rgb32 &bitmap );

	psx_gpu_debug m_debug;
#endif
};

class cxd8514q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8514q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, psxcpu_device *cpu);
	cxd8514q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8538q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8538q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, psxcpu_device *cpu);
	cxd8538q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8561q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8561q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, psxcpu_device *cpu);
	cxd8561q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8561bq_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8561bq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, psxcpu_device *cpu);
	cxd8561bq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8561cq_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8561cq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, psxcpu_device *cpu);
	cxd8561cq_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

class cxd8654q_device : public psxgpu_device
{
public:
	// construction/destruction
	cxd8654q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, psxcpu_device *cpu);
	cxd8654q_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
};

#endif // MAME_VIDEO_PSX_H
