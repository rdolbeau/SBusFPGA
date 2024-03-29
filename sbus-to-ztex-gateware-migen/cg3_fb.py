from migen import *
from migen.genlib.fifo import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect import stream
from litex.soc.interconnect import wishbone
from litex.soc.cores.code_tmds import TMDSEncoder

from litex.build.io import SDROutput, DDROutput

from litex.soc.cores.video import *

from math import ceil;

# " 108000000,71808,76,32,128,192,1152,2,8,33,900,COLOR,0OFFSET"
cg3_timings = {
    "1152x900@76Hz": {
        "pix_clk"       : 108e6,
        "h_active"      : 1152,
        "h_blanking"    : 352,
        "h_sync_offset" : 32,
        "h_sync_width"  : 128,
        "v_active"      : 900,
        "v_blanking"    : 43,
        "v_sync_offset" : 2,
        "v_sync_width"  : 8,
    },
}

cmap_layout = [
    ("color", 2),
    ("address", 8),
    ("data", 8),
]
omap_layout = [
    ("color", 2),
    ("address", 2),
    ("data", 8),
]

def cg3_rounded_size(hres, vres):
    mib = int(ceil(((hres * vres) + 0) / 1048576))
    if (mib == 3):
        mib = 4
    if (mib > 4 and mib < 8):
        mib = 8
    if (mib > 8 or mib < 1):
        print(f"{mib} mebibytes framebuffer not supported")
        assert(False)
    return int(1048576 * mib)

class VideoFrameBuffer256c(Module, AutoCSR):
    """Video FrameBuffer256c"""
    def __init__(self, dram_port, upd_clut_fifo = None, hres=800, vres=600, base=0x00000000, fifo_depth=65536, clock_domain="sys", clock_faster_than_sys=False, hwcursor=False, upd_overlay_fifo=False, upd_omap_fifo=False):
        clut = Array(Array(Signal(8, reset = (255-i)) for i in range(0, 256)) for j in range(0, 3))
        
        upd_clut_fifo_dout = Record(cmap_layout)
        self.comb += upd_clut_fifo_dout.raw_bits().eq(upd_clut_fifo.dout)

        if (hwcursor):
            upd_omap_fifo_dout = Record(omap_layout)
            self.comb += upd_omap_fifo_dout.raw_bits().eq(upd_omap_fifo.dout)

        print(f"FRAMEBUFFER: dram_port.data_width = {dram_port.data_width}, {hres}x{vres}, 0x{base:x}, in {clock_domain}, clock_faster_than_sys={clock_faster_than_sys}")

        vga_sync = getattr(self.sync, clock_domain)
        vga_sync += [
            If(upd_clut_fifo.readable,
               upd_clut_fifo.re.eq(1),
               clut[upd_clut_fifo_dout.color][upd_clut_fifo_dout.address].eq(upd_clut_fifo_dout.data),
            ).Else(
               upd_clut_fifo.re.eq(0),
            )
        ]

        if (hwcursor):
            self.vtg_sink  = vtg_sink = stream.Endpoint(video_timing_hwcursor_layout)
            overlay = Array(Array(Array(Signal(1) for x in range(0,32)) for y in range(0,32)) for i in range(0, 2))
            omap = Array(Array(Signal(8, reset = (255-i)) for i in range(0, 4)) for j in range(0, 3))
            vga_sync += [
                If(upd_overlay_fifo.readable,
                    upd_overlay_fifo.re.eq(1),
                    [ overlay[upd_overlay_fifo.dout[0]][upd_overlay_fifo.dout[1:6]][x].eq(upd_overlay_fifo.dout[6+x]) for x in range(0, 32)],
                    ).Else(
                        upd_overlay_fifo.re.eq(0),
                    )
            ]
            vga_sync += [
                If(upd_omap_fifo.readable,
                   upd_omap_fifo.re.eq(1),
                   omap[upd_omap_fifo_dout.color][upd_omap_fifo_dout.address].eq(upd_omap_fifo_dout.data),
                ).Else(
                    upd_omap_fifo.re.eq(0),
                )
            ]
        else:
            self.vtg_sink  = vtg_sink = stream.Endpoint(video_timing_layout)
        self.source    = source   = stream.Endpoint(video_data_layout)
        self.underflow = Signal()

        #source_buf_ready = Signal()
        source_buf_valid = Signal()
        source_buf_de = Signal()
        source_buf_hsync = Signal()
        source_buf_vsync = Signal()
        data_buf = Signal(8)
        if (hwcursor):
            hwcursor_buf = Signal()
            hwcursorx_buf = Signal(5)
            hwcursory_buf = Signal(5)
        
        #source_out_ready = Signal()
        source_out_valid = Signal()
        source_out_de = Signal()
        source_out_hsync = Signal()
        source_out_vsync = Signal()
        source_out_r = Signal(8)
        source_out_g = Signal(8)
        source_out_b = Signal(8)

        # # #

        # Video DMA.
        from litedram.frontend.dma import LiteDRAMDMAReader
        self.submodules.dma = LiteDRAMDMAReader(dram_port, fifo_depth=fifo_depth//(dram_port.data_width//8), fifo_buffered=True)
        self.dma.add_csr(
            default_base   = base,
            default_length = hres*vres*8//8, # 8-bit PseudoColor
            default_enable = 0,
            default_loop   = 1
        )

        # If DRAM Data Width > 8-bit and Video clock is faster than sys_clk:
        if (dram_port.data_width > 8) and clock_faster_than_sys:
            # Do Clock Domain Crossing first...
            self.submodules.cdc = stream.ClockDomainCrossing([("data", dram_port.data_width)], cd_from="sys", cd_to=clock_domain)
            self.comb += self.dma.source.connect(self.cdc.sink)
            # ... and then Data-Width Conversion.
            self.submodules.conv = ClockDomainsRenamer({"sys": clock_domain})(stream.Converter(dram_port.data_width, 8))
            self.comb += self.cdc.source.connect(self.conv.sink)
            video_pipe_source = self.conv.source
        # Elsif DRAM Data Widt < 8-bit or Video clock is slower than sys_clk:
        else:
            # Do Data-Width Conversion first...
            self.submodules.conv = stream.Converter(dram_port.data_width, 8)
            self.comb += self.dma.source.connect(self.conv.sink)
            # ... and then Clock Domain Crossing.
            self.submodules.cdc = stream.ClockDomainCrossing([("data", 8)], cd_from="sys", cd_to=clock_domain)
            self.comb += self.conv.source.connect(self.cdc.sink)
            video_pipe_source = self.cdc.source
        
        # Video Generation.
        self.comb += [
            vtg_sink.ready.eq(1),
            If(vtg_sink.valid & vtg_sink.de,
               source_buf_valid.eq(video_pipe_source.valid),
               #video_pipe_source.ready.eq(source_buf_ready),# ready flow the other way
               video_pipe_source.connect(source, keep={"ready"}), # source.ready is set to 1 by the sink anyway, bypass the cycle delay
               #vtg_sink.ready.eq(source_buf_valid & source_buf_ready),
               vtg_sink.ready.eq(source_buf_valid & source.ready),
            ),
            source_buf_de.eq(vtg_sink.de),
            source_buf_hsync.eq(vtg_sink.hsync),
            source_buf_vsync.eq(vtg_sink.vsync),
            data_buf.eq(video_pipe_source.data),
        ]
        if (hwcursor):
            self.comb += [
                hwcursor_buf.eq(vtg_sink.hwcursor),
                hwcursorx_buf.eq(vtg_sink.hwcursorx),
                hwcursory_buf.eq(vtg_sink.hwcursory),
        ]

        if (hwcursor):
            source_mid_valid = Signal()
            source_mid_de = Signal()
            source_mid_hsync = Signal()
            source_mid_vsync = Signal()
            data_mid = Signal(8)
        
            hwcursor_color_idx = Signal(2)

            # first cycle, buffer everything and look up the cursor overlay color
            vga_sync += [
                source_mid_de.eq(source_buf_de),
                source_mid_hsync.eq(source_buf_hsync),
                source_mid_vsync.eq(source_buf_vsync),
                source_mid_valid.eq(source_buf_valid),
                data_mid.eq(data_buf),
                If(hwcursor_buf,
                   hwcursor_color_idx.eq(Cat(overlay[0][hwcursory_buf][hwcursorx_buf], overlay[1][hwcursory_buf][hwcursorx_buf])),
                ).Else(
                    hwcursor_color_idx.eq(0),
                )
            ]

            #second cycle, produce the pixel by doing CLUT lookup
            vga_sync += [
                source_out_de.eq(source_mid_de),
                source_out_hsync.eq(source_mid_hsync),
                source_out_vsync.eq(source_mid_vsync),
                source_out_valid.eq(source_mid_valid),
                #source_buf_ready.eq(source_out_ready), # ready flow the other way
                If(hwcursor_color_idx != 0,
                   source_out_r.eq(omap[0][hwcursor_color_idx]),
                   source_out_g.eq(omap[1][hwcursor_color_idx]),
                   source_out_b.eq(omap[2][hwcursor_color_idx]),
                ).Elif(source_mid_de,
                       source_out_r.eq(clut[0][data_mid]),
                       source_out_g.eq(clut[1][data_mid]),
                       source_out_b.eq(clut[2][data_mid])
                ).Else(
                    source_out_r.eq(0),
                    source_out_g.eq(0),
                    source_out_b.eq(0)
                )
                ]
        else:
            vga_sync += [
                source_out_de.eq(source_buf_de),
                source_out_hsync.eq(source_buf_hsync),
                source_out_vsync.eq(source_buf_vsync),
                source_out_valid.eq(source_buf_valid),
                #source_buf_ready.eq(source_out_ready), # ready flow the other way
                If(source_buf_de,
                   source_out_r.eq(clut[0][data_buf]),
                   source_out_g.eq(clut[1][data_buf]),
                   source_out_b.eq(clut[2][data_buf])
                ).Else(
                    source_out_r.eq(0),
                    source_out_g.eq(0),
                    source_out_b.eq(0)
                )
            ]

        self.comb += [
            source.de.eq(source_out_de),
            source.hsync.eq(source_out_hsync),
            source.vsync.eq(source_out_vsync),
            source.valid.eq(source_out_valid),
            #source_out_ready.eq(source.ready), # ready flow the other way
            source.r.eq(source_out_r),
            source.g.eq(source_out_g),
            source.b.eq(source_out_b),
        ]

        # Underflow.
        self.comb += self.underflow.eq(~source.valid)

        
class cg3(Module, AutoCSR):
    def __init__(self, soc, phy=None, timings = None, clock_domain="sys"):

        # 2 bits for color (0/r, 1/g, 2/b), 8 for @ and 8 for value
        self.submodules.upd_cmap_fifo = upd_cmap_fifo = ClockDomainsRenamer({"read": "vga", "write": "sys"})(AsyncFIFOBuffered(width=layout_len(cmap_layout), depth=8))
        upd_cmap_fifo_din = Record(cmap_layout)
        self.comb += self.upd_cmap_fifo.din.eq(upd_cmap_fifo_din.raw_bits())
        
        name = "video_framebuffer"
        # near duplicate of plaform.add_video_framebuffer
        # Video Timing Generator.
        vtg = VideoTimingGenerator(default_video_timings=timings if isinstance(timings, str) else timings[1])
        vtg = ClockDomainsRenamer(clock_domain)(vtg)
        setattr(self.submodules, f"{name}_vtg", vtg)

        # Video FrameBuffer.
        timings = timings if isinstance(timings, str) else timings[0]
        base = soc.mem_map.get(name)
        print(f"CG3: visible memory at {base:x}")
        hres = int(timings.split("@")[0].split("x")[0])
        vres = int(timings.split("@")[0].split("x")[1])
        freq = vtg.video_timings["pix_clk"]
        print(f"CG3: using {hres} x {vres}, {freq/1e6} MHz pixclk")
        vfb = VideoFrameBuffer256c(dram_port = soc.sdram.crossbar.get_port(),
                                   upd_clut_fifo = upd_cmap_fifo,
                                   hres = hres,
                                   vres = vres,
                                   base = base,
                                   clock_domain = clock_domain,
                                   clock_faster_than_sys = (vtg.video_timings["pix_clk"] > soc.sys_clk_freq))
        setattr(self.submodules, name, vfb)

        # Connect Video Timing Generator to Video FrameBuffer.
        self.comb += vtg.source.connect(vfb.vtg_sink)

        # Connect Video FrameBuffer to Video PHY.
        self.comb += vfb.source.connect(phy if isinstance(phy, stream.Endpoint) else phy.sink)

        # Constants.
        soc.add_constant("VIDEO_FRAMEBUFFER_BASE", base)
        soc.add_constant("VIDEO_FRAMEBUFFER_HRES", hres)
        soc.add_constant("VIDEO_FRAMEBUFFER_VRES", vres)

        # cg3 registers
        # struct bt_regs {
        # 	u_int	bt_addr;		/* map address register */
        # 	u_int	bt_cmap;		/* colormap data register */
        # 	u_int	bt_ctrl;		/* control register */
        # 	u_int	bt_omap;		/* overlay (cursor) map register */
        # };
        # /*
        #  * Sbus framebuffer control look like this (usually at offset 0x400000).
        #  */
        # struct fbcontrol {
        # 	struct	bt_regs fbc_dac;
        # 	u_char	fbc_ctrl;
        # 	u_char	fbc_status;
        # 	u_char	fbc_cursor_start;
        # 	u_char	fbc_cursor_end;
        # 	u_char	fbc_vcontrol[12];	/* 12 bytes of video timing goo */
        # };

        self.bus = bus = wishbone.Interface()

        bt_cmap_idx = Signal(8)
        bt_cmap_state = Signal(2)
        bt_cmap_buf = Signal(24)

        fbc_ctrl = Signal(8, reset = 0x60) # FBC_VENAB | FBC_TIMING  # 0x10 ?
        hres_to_sense = {
            "default": 0x30, # 1152x900
            1024: 0x10,
            1152: 0x30,
            1280: 0x40,
        };
        fbc_status = Signal(8, reset = (hres_to_sense[hres] | 0x01)) # 1280x1024 color # 0x11 ?
        fbc_cursor_start = Signal(8) # 0x12 ?
        fbc_cursor_end = Signal(8) # 0x13 ?
        fbc_vcontrol = Array(Signal(8) for a in range(0, 3))

        # current cmap logic for the CG3
        # (the CG6 takes 32 bits write but only use the top 8 bits, for bt_addr & bt_cmap
        #  also it uses the BT HW cursor (though probably not in the console?) )

        self.submodules.wishbone_fsm = wishbone_fsm = FSM(reset_state = "Reset")
        wishbone_fsm.act("Reset",
                         NextValue(bus.ack, 0),
                         NextState("Idle"))
        wishbone_fsm.act("Idle",
                         If(bus.cyc & bus.stb & bus.we & ~bus.ack & upd_cmap_fifo.writable, #write
                            # FIXME: should check for prefix?
                            Case(bus.adr[0:3], {
                                # bt_addr
                                0: [ NextValue(bt_cmap_idx, bus.dat_w[0:8]),
                                     NextValue(bt_cmap_state, 0),
                                ],
                                # bt_cmap
                                1: [ Case(bus.sel, { 
                                    "default": [ NextValue(bt_cmap_buf, bus.dat_w[0:24]),
                                                 upd_cmap_fifo.we.eq(1),
                                                 upd_cmap_fifo_din.color.eq(bt_cmap_state),
                                                 upd_cmap_fifo_din.address.eq(bt_cmap_idx),
                                                 upd_cmap_fifo_din.data.eq(bus.dat_w[24:32]),
                                                 Case(bt_cmap_state, {
                                                     0: [ NextValue(bt_cmap_state, 1), ],
                                                     1: [ NextValue(bt_cmap_state, 2), ],
                                                     2: [ NextValue(bt_cmap_state, 0), NextValue(bt_cmap_idx, (bt_cmap_idx+1) & 0xFF), ],
                                                     "default":  NextValue(bt_cmap_state, 0),
                                                 }),
                                                 NextState("cmap_a"),
                                    ],
                                    # will sel be 1 or 8 ?
                                    1: [ upd_cmap_fifo.we.eq(1),
                                         upd_cmap_fifo_din.color.eq(bt_cmap_state),
                                         upd_cmap_fifo_din.address.eq(bt_cmap_idx),
                                         upd_cmap_fifo_din.data.eq(bus.dat_w[24:32]),
                                         Case(bt_cmap_state, {
                                             0: [ NextValue(bt_cmap_state, 1), ],
                                             1: [ NextValue(bt_cmap_state, 2), ],
                                             2: [ NextValue(bt_cmap_state, 0), NextValue(bt_cmap_idx, (bt_cmap_idx+1) & 0xFF), ],
                                             "default":  NextValue(bt_cmap_state, 0),
                                         })
                                    ],
                                    8: [ upd_cmap_fifo.we.eq(1),
                                         upd_cmap_fifo_din.color.eq(bt_cmap_state),
                                         upd_cmap_fifo_din.address.eq(bt_cmap_idx),
                                         upd_cmap_fifo_din.data.eq(bus.dat_w[24:32]),
                                         Case(bt_cmap_state, {
                                             0: [ NextValue(bt_cmap_state, 1), ],
                                             1: [ NextValue(bt_cmap_state, 2), ],
                                             2: [ NextValue(bt_cmap_state, 0), NextValue(bt_cmap_idx, (bt_cmap_idx+1) & 0xFF), ],
                                             "default":  NextValue(bt_cmap_state, 0),
                                         })
                                    ],
                                })],
                                # bt_ctrl: unused ??
                                2: [],
                                # bt_omap: unused ??
                                3: [],
                                # fbc_ctrl & friends: 4 in one go
                                # should be byte-accessed
                                # CHECKME: byte ordering
                                4: [ Case(bus.sel, {
                                    8: [ NextValue(fbc_ctrl, bus.dat_w[24:32]),],
                                    4: [ NextValue(fbc_status, fbc_status & 0x7F),], #FIXME: if high bit set, cancel interrupt
                                    2: [ NextValue(fbc_cursor_start, bus.dat_w[24:32]),],
                                    1: [ NextValue(fbc_cursor_end, bus.dat_w[24:32]),],
                                }),
                                ],
                                5: [NextValue(fbc_vcontrol[0], (bus.dat_w & Cat(Replicate(bus.sel[3], 8), Replicate(bus.sel[2], 8), Replicate(bus.sel[1], 8), Replicate(bus.sel[0], 8))) | (fbc_vcontrol[0] & ~Cat(Replicate(bus.sel[3], 8), Replicate(bus.sel[2], 8), Replicate(bus.sel[1], 8), Replicate(bus.sel[0], 8)))) ],
                                6: [NextValue(fbc_vcontrol[1], (bus.dat_w & Cat(Replicate(bus.sel[3], 8), Replicate(bus.sel[2], 8), Replicate(bus.sel[1], 8), Replicate(bus.sel[0], 8))) | (fbc_vcontrol[1] & ~Cat(Replicate(bus.sel[3], 8), Replicate(bus.sel[2], 8), Replicate(bus.sel[1], 8), Replicate(bus.sel[0], 8)))) ],
                                7: [NextValue(fbc_vcontrol[2], (bus.dat_w & Cat(Replicate(bus.sel[3], 8), Replicate(bus.sel[2], 8), Replicate(bus.sel[1], 8), Replicate(bus.sel[0], 8))) | (fbc_vcontrol[2] & ~Cat(Replicate(bus.sel[3], 8), Replicate(bus.sel[2], 8), Replicate(bus.sel[1], 8), Replicate(bus.sel[0], 8)))) ],
                            }),
                            NextValue(bus.ack, 1),
                         ).Elif(bus.cyc & bus.stb & ~bus.we & ~bus.ack, #read
                                Case(bus.adr[0:3], {
                                # bt_addr
                                0: [ NextValue(bus.dat_r, 0) ],
                                # bt_cmap
                                1: [ NextValue(bus.dat_r, 0)],
                                # bt_ctrl: unused ??
                                2: [ NextValue(bus.dat_r, 0)],
                                # bt_omap: unused ??
                                3: [ NextValue(bus.dat_r, 0)],
                                # fbc_ctrl & friends: 4 in one go
                                # should be byte-accessed
                                # CHECKME: byte ordering
                                4: [ NextValue(bus.dat_r, Cat(fbc_cursor_end, fbc_cursor_start, fbc_status, fbc_ctrl))],
                                5: [ NextValue(bus.dat_r, fbc_vcontrol[0])],
                                6: [ NextValue(bus.dat_r, fbc_vcontrol[1])],
                                7: [ NextValue(bus.dat_r, fbc_vcontrol[2])],
                            }),
                            NextValue(bus.ack, 1),
                         ).Else(
                             NextValue(bus.ack, 0),
                         )
        )

        wishbone_fsm.act("cmap_a",
                         If(upd_cmap_fifo.writable,
                            upd_cmap_fifo.we.eq(1),
                            upd_cmap_fifo_din.color.eq(bt_cmap_state),
                            upd_cmap_fifo_din.address.eq(bt_cmap_idx),
                            upd_cmap_fifo_din.data.eq(bus.dat_w[16:24]),
                            Case(bt_cmap_state, {
                                0: [ NextValue(bt_cmap_state, 1), ],
                                1: [ NextValue(bt_cmap_state, 2), ],
                                2: [ NextValue(bt_cmap_state, 0), NextValue(bt_cmap_idx, (bt_cmap_idx+1) & 0xFF), ],
                                "default":  NextValue(bt_cmap_state, 0),
                            }),
                            NextState("cmap_b")))
        wishbone_fsm.act("cmap_b",
                         If(upd_cmap_fifo.writable,
                            upd_cmap_fifo.we.eq(1),
                            upd_cmap_fifo_din.color.eq(bt_cmap_state),
                            upd_cmap_fifo_din.address.eq(bt_cmap_idx),
                            upd_cmap_fifo_din.data.eq(bus.dat_w[8:16]),
                            Case(bt_cmap_state, {
                                0: [ NextValue(bt_cmap_state, 1), ],
                                1: [ NextValue(bt_cmap_state, 2), ],
                                2: [ NextValue(bt_cmap_state, 0), NextValue(bt_cmap_idx, (bt_cmap_idx+1) & 0xFF), ],
                                "default":  NextValue(bt_cmap_state, 0),
                            }),
                            NextState("cmap_c")))
        wishbone_fsm.act("cmap_c",
                         If(upd_cmap_fifo.writable,
                            upd_cmap_fifo.we.eq(1),
                            upd_cmap_fifo_din.color.eq(bt_cmap_state),
                            upd_cmap_fifo_din.address.eq(bt_cmap_idx),
                            upd_cmap_fifo_din.data.eq(bus.dat_w[0:8]),
                            Case(bt_cmap_state, {
                                0: [ NextValue(bt_cmap_state, 1), ],
                                1: [ NextValue(bt_cmap_state, 2), ],
                                2: [ NextValue(bt_cmap_state, 0), NextValue(bt_cmap_idx, (bt_cmap_idx+1) & 0xFF), ],
                                "default":  NextValue(bt_cmap_state, 0),
                            }),
                            NextState("Idle")))
