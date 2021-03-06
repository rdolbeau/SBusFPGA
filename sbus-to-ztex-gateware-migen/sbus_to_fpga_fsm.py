
from migen import *
from migen.fhdl.specials import Tristate

SIZ_WORD = 0x0
SIZ_BYTE = 0x1
SIZ_HWORD = 0x2
SIZ_EXT = 0x3
SIZ_BURST4 = 0x4
SIZ_BURST8 = 0x5
SIZ_BURST16 = 0x6
SIZ_BURST2 = 0x7

ACK_IDLE = 0x7
ACK_ERR = 0x6
ACK_BYTE = 0x5
ACK_RERUN = 0x4
ACK_WORD = 0x3
ACK_DWORD = 0x2
ACK_HWORD = 0x1
ACK_RECV = 0x0

ADDR_PHYS_HIGH = 27
ADDR_PHYS_LOW = 0
ADDR_PFX_HIGH = ADDR_PHYS_HIGH
ADDR_PFX_LOW = 16 ## 64 KiB per prefix
ADDR_PFX_LENGTH = 12 #(1 + ADDR_PFX_HIGH - ADDR_PFX_LOW)
ROM_ADDR_PFX = Signal(12, reset = 0)
WISHBONE_CSR_ADDR_PFX = Signal(12, reset = 4)
USBOHCI_ADDR_PFX = Signal(12, reset = 8)
SRAM_ADDR_PFX = Signal(12, reset = 9)

wishbone_default_timeout = 120 ## must be > sbus_default_timeout
sbus_default_timeout = 100 ## must be below 127 as we can wait twice on it inside the 255 cycles

def siz_is_word(siz):
    return (SIZ_WORD == siz) | (SIZ_BURST2 == siz) | (SIZ_BURST4 == siz) | (SIZ_BURST8 == siz) | (SIZ_BURST16 == siz)

# FIXME: this doesn't work. Verilog aways use value[0:4]
#def _index_with_wrap(counter, limit_m1, value):
#    if (limit_m1 == 0):
#        return value[0:4]
#    elif (limit_m1 == 1):
#        return Cat((value + counter)[0:1], value[1:4])
#    elif (limit_m1 == 3):
#        return Cat((value + counter)[0:2], value[2:4])
#    elif (limit_m1 == 7):
#        return Cat((value + counter)[0:3], value[3:4])
#    elif (limit_m1 == 15):
#        return (value + counter)[0:4]
#    return value[0:4]

def index_with_wrap(counter, limit_m1, value):
    return ((value+counter) & limit_m1)[0:4] | (value&(~limit_m1))[0:4]

# FIXME: this doesn't work. Verilog aways use 1
def siz_to_burst_size_m1(siz):
    if (SIZ_WORD == siz):
        return 0
    elif (SIZ_BURST2 == siz):
        return 1
    elif (SIZ_BURST4 == siz):
        return 3
    elif (SIZ_BURST8 == siz):
        return 7
    elif (SIZ_BURST16 == siz):
        return 15
    return 1

class LedDisplay(Module):
    def __init__(self, pads):
        n = len(pads)
        self.value = Signal(40, reset = 0x0018244281)
        old_value = Signal(40)
        self.display = Signal(8)
        self.comb += pads.eq(self.display)
        
        self.submodules.fsm = fsm = FSM(reset_state="Reset")
        time_counter = Signal(32, reset = 0)
        blink_counter = Signal(4, reset = 0)
        fsm.act("Reset",
                NextValue(time_counter, 25000000//2),
                NextValue(blink_counter, 0),
                NextValue(self.display, self.value[0:8]),
                NextValue(old_value, self.value),
                NextState("Byte0"))
        fsm.act("Quick",
                If(old_value != self.value,
                    NextState("Reset")
                ).Elif(time_counter == 0,
                   If(blink_counter == 0,
                       NextValue(time_counter, 25000000//2),
                       NextValue(self.display, self.value[0:8]),
                       NextState("Byte0")
                   ).Else(
                       NextValue(self.display, ~self.display),
                       NextValue(time_counter, 25000000//10),
                       NextValue(blink_counter, blink_counter - 1)
                   )
                ).Else(
                    NextValue(time_counter, time_counter - 1)
                )
        )
        fsm.act("Byte0",
                If(old_value != self.value,
                    NextState("Reset")
                ).Elif(time_counter == 0,
                       NextValue(time_counter, 25000000//2),
                       NextValue(self.display, self.value[8:16]),
                       NextState("Byte1")
                ).Else(
                    NextValue(time_counter, time_counter - 1)
                )
        )
        fsm.act("Byte1",
                If(old_value != self.value,
                    NextState("Reset")
                ).Elif(time_counter == 0,
                       NextValue(time_counter, 25000000//2),
                       NextValue(self.display, self.value[16:24]),
                       NextState("Byte2")
                ).Else(
                    NextValue(time_counter, time_counter - 1)
                )
        )
        fsm.act("Byte2",
                If(old_value != self.value,
                    NextState("Reset")
                ).Elif(time_counter == 0,
                       NextValue(time_counter, 25000000//2),
                       NextValue(self.display, self.value[24:32]),
                       NextState("Byte3")
                ).Else(
                    NextValue(time_counter, time_counter - 1)
                )
        )
        fsm.act("Byte3",
                If(old_value != self.value,
                    NextState("Reset")
                ).Elif(time_counter == 0,
                       NextValue(time_counter, 25000000//2),
                       NextValue(self.display, self.value[32:40]),
                       NextState("Byte4")
                ).Else(
                    NextValue(time_counter, time_counter - 1)
                )
        )
        fsm.act("Byte4",
                If(old_value != self.value,
                    NextState("Reset")
                ).Elif(time_counter == 0,
                       NextValue(time_counter, 25000000//10),
                       NextValue(blink_counter, 10),
                       NextValue(self.display, 0x00),
                       NextState("Quick")
                ).Else(
                    NextValue(time_counter, time_counter - 1)
                )
        )

LED_PARITY=0x11
LED_ADDRESS=0x12
LED_UNKNOWNREQ=0x14
LED_RERUN=0x8
LED_RERUN_WRITE=0x4
LED_RERUN_WORD=0x2
LED_RERUN_LATE=0x1

LED_M_WRITE = 0x10
LED_M_READ = 0x20
LED_M_CACHE = 0x40
        
class SBusFPGABus(Module):
    def __init__(self, platform, hold_reset, wishbone_slave, wishbone_master):
        self.platform = platform
        self.hold_reset = hold_reset

        self.wishbone_slave = wishbone_slave
        self.wishbone_master = wishbone_master
        
        pad_SBUS_DATA_OE_LED = platform.request("SBUS_DATA_OE_LED")
        SBUS_DATA_OE_LED_o = Signal()
        self.comb += pad_SBUS_DATA_OE_LED.eq(SBUS_DATA_OE_LED_o)
        ##pad_SBUS_DATA_OE_LED_2 = platform.request("SBUS_DATA_OE_LED_2")
        ##SBUS_DATA_OE_LED_2_o = Signal()
        ##self.comb += pad_SBUS_DATA_OE_LED_2.eq(SBUS_DATA_OE_LED_2_o)

        #leds = Signal(7, reset=0x00)
        #self.comb += platform.request("user_led", 0).eq(leds[0])
        #self.comb += platform.request("user_led", 1).eq(leds[1])
        #self.comb += platform.request("user_led", 2).eq(leds[2])
        #self.comb += platform.request("user_led", 3).eq(leds[3])
        #self.comb += platform.request("user_led", 4).eq(leds[4])
        #self.comb += platform.request("user_led", 5).eq(leds[5])
        #self.comb += platform.request("user_led", 6).eq(leds[6])
        ##self.comb += platform.request("user_led", 7).eq(leds[7])
        
        #pad_SBUS_3V3_CLK = platform.request("SBUS_3V3_CLK")
        pad_SBUS_3V3_ASs = platform.request("SBUS_3V3_ASs")
        pad_SBUS_3V3_BGs = platform.request("SBUS_3V3_BGs")
        pad_SBUS_3V3_BRs = platform.request("SBUS_3V3_BRs")
        pad_SBUS_3V3_ERRs = platform.request("SBUS_3V3_ERRs")
        #pad_SBUS_3V3_RSTs = platform.request("SBUS_3V3_RSTs")
        pad_SBUS_3V3_SELs = platform.request("SBUS_3V3_SELs")
        #pad_SBUS_3V3_INT1s = platform.request("SBUS_3V3_INT1s")
        pad_SBUS_3V3_INT7s = platform.request("SBUS_3V3_INT7s")
        pad_SBUS_3V3_PPRD = platform.request("SBUS_3V3_PPRD")
        pad_SBUS_OE = platform.request("SBUS_OE")
        pad_SBUS_3V3_ACKs = platform.request("SBUS_3V3_ACKs")
        pad_SBUS_3V3_SIZ = platform.request("SBUS_3V3_SIZ")
        pad_SBUS_3V3_D = platform.request("SBUS_3V3_D")
        pad_SBUS_3V3_PA = platform.request("SBUS_3V3_PA")
        assert len(pad_SBUS_3V3_D) == 32, "len(pad_SBUS_3V3_D) should be 32"
        assert len(pad_SBUS_3V3_PA) == 28, "len(pad_SBUS_3V3_PA) should be 28"

        sbus_oe_data = Signal(reset=0)
        sbus_oe_slave_in = Signal(reset=0)
        sbus_oe_master_in = Signal(reset=0)
        #sbus_oe_int1 = Signal(reset=0)
        sbus_oe_int7 = Signal(reset=0)
        #sbus_oe_master_br = Signal(reset=0)

        sbus_last_pa = Signal(28)
        burst_index = Signal(4)
        burst_counter = Signal(4)
        burst_limit_m1 = Signal(4)

        #SBUS_3V3_CLK = Signal()
        SBUS_3V3_ASs_i = Signal(reset=1)
        self.comb += SBUS_3V3_ASs_i.eq(pad_SBUS_3V3_ASs)
        SBUS_3V3_BGs_i = Signal(reset=1)
        self.comb += SBUS_3V3_BGs_i.eq(pad_SBUS_3V3_BGs)
        SBUS_3V3_BRs_o = Signal(reset=1)
        #self.specials += Tristate(pad_SBUS_3V3_BRs, SBUS_3V3_BRs_o, sbus_oe_master_br, None)
        self.comb += pad_SBUS_3V3_BRs.eq(SBUS_3V3_BRs_o)
        SBUS_3V3_ERRs_i = Signal()
        SBUS_3V3_ERRs_o = Signal()
        self.specials += Tristate(pad_SBUS_3V3_ERRs, SBUS_3V3_ERRs_o, sbus_oe_master_in, SBUS_3V3_ERRs_i)
        #SBUS_3V3_RSTs = Signal()
        SBUS_3V3_SELs_i = Signal(reset=1)
        self.comb += SBUS_3V3_SELs_i.eq(pad_SBUS_3V3_SELs)
        #SBUS_3V3_INT1s_o = Signal(reset=1)
        #self.specials += Tristate(pad_SBUS_3V3_INT1s, SBUS_3V3_INT1s_o, sbus_oe_int1, None)
        SBUS_3V3_INT7s_o = Signal(reset=1)
        self.specials += Tristate(pad_SBUS_3V3_INT7s, SBUS_3V3_INT7s_o, sbus_oe_int7, None)
        SBUS_3V3_PPRD_i = Signal()
        SBUS_3V3_PPRD_o = Signal()
        self.specials += Tristate(pad_SBUS_3V3_PPRD, SBUS_3V3_PPRD_o, sbus_oe_slave_in, SBUS_3V3_PPRD_i)
        #SBUS_OE_o = Signal()
        self.comb += pad_SBUS_OE.eq(self.hold_reset)
        SBUS_3V3_ACKs_i = Signal(3)
        SBUS_3V3_ACKs_o = Signal(3)
        self.specials += Tristate(pad_SBUS_3V3_ACKs, SBUS_3V3_ACKs_o, sbus_oe_master_in, SBUS_3V3_ACKs_i)
        SBUS_3V3_SIZ_i = Signal(3)
        SBUS_3V3_SIZ_o = Signal(3)
        self.specials += Tristate(pad_SBUS_3V3_SIZ, SBUS_3V3_SIZ_o, sbus_oe_slave_in, SBUS_3V3_SIZ_i)
        SBUS_3V3_D_i = Signal(32)
        SBUS_3V3_D_o = Signal(32)
        self.specials += Tristate(pad_SBUS_3V3_D, SBUS_3V3_D_o, sbus_oe_data, SBUS_3V3_D_i)
        SBUS_3V3_PA_i = Signal(28)
        self.comb += SBUS_3V3_PA_i.eq(pad_SBUS_3V3_PA)

        p_data = Signal(32) # data to read/write

        data_read_addr = Signal(30) # first addr of req. when reading from WB
        data_read_enable = Signal() # start enqueuing req. to read from WB

        master_data = Signal(32) # could be merged with p_data
        master_addr = Signal(30) # could be meged with data_read_addr
        master_size = Signal(4)
        master_idx = Signal(2)

        master_we = Signal()

        sbus_wishbone_le = Signal()

        wishbone_master_timeout = Signal(6)
        wishbone_slave_timeout = Signal(6)
        sbus_slave_timeout = Signal(6)

        sbus_master_throttle = Signal(4)
        
        #self.submodules.led_display = LedDisplay(platform.request_all("user_led"))
        
        self.sync += platform.request("user_led", 4).eq(self.wishbone_slave.cyc)
        #self.sync += platform.request("user_led", 5).eq(self.wishbone_slave.stb)
        #self.sync += platform.request("user_led", 6).eq(self.wishbone_slave.we)
        #self.sync += platform.request("user_led", 7).eq(self.wishbone_slave.ack)
        #self.sync += platform.request("user_led", 4).eq(self.wishbone_slave.err)
        #led4 = platform.request("user_led", 4)
        #led5 = platform.request("user_led", 5)
        #led6 = platform.request("user_led", 6)
        #led7 = platform.request("user_led", 7)

        led0123 = Signal(4)
        self.sync += platform.request("user_led", 0).eq(led0123[0])
        self.sync += platform.request("user_led", 1).eq(led0123[1])
        self.sync += platform.request("user_led", 2).eq(led0123[2])
        self.sync += platform.request("user_led", 3).eq(led0123[3])

        #self.sync += platform.request("user_led", 0).eq(self.wishbone_master.cyc)
        #self.sync += platform.request("user_led", 1).eq(self.wishbone_master.stb)
        #self.sync += platform.request("user_led", 2).eq(self.wishbone_master.we)
        #self.sync += platform.request("user_led", 3).eq(self.wishbone_master.ack)
        #self.sync += platform.request("user_led", 4).eq(~SBUS_3V3_SELs_i)
        
        #self.sync += platform.request("user_led", 4).eq(self.wishbone_master.cyc)
        #self.sync += platform.request("user_led", 5).eq(~SBUS_3V3_ASs_i)
        #self.sync += platform.request("user_led", 6).eq(wishbone_master_timeout == 0)
        #led7 = platform.request("user_led", 7)

        #self.sync += platform.request("user_led", 5).eq(self.wishbone_slave.cyc)
        #self.sync += platform.request("user_led", 6).eq(~SBUS_3V3_BRs_o)
        #self.sync += platform.request("user_led", 7).eq(~SBUS_3V3_BGs_i)
        self.sync += SBUS_DATA_OE_LED_o.eq(~SBUS_3V3_BGs_i),

        #cycle_counter = Signal(8, reset = 0)
        #self.sync += cycle_counter.eq(cycle_counter + 1)
        #cycle_busmaster = Signal(8, reset = 0)
        #self.sync += If(cycle_counter != 0,
        #                cycle_busmaster.eq(cycle_busmaster + ~SBUS_3V3_BGs_i)).Else(
        #                    cycle_busmaster.eq(0))
        #self.sync += If(cycle_counter == 0,
        #                platform.request("user_led", 0).eq(cycle_busmaster[4]),
        #                platform.request("user_led", 1).eq(cycle_busmaster[5]),
        #                platform.request("user_led", 2).eq(cycle_busmaster[6]),
        #                platform.request("user_led", 3).eq(cycle_busmaster[7]))

        self.master_read_buffer_data = Array(Signal(32) for a in range(4))
        self.master_read_buffer_addr = Signal(28)
        self.master_read_buffer_done = Array(Signal() for a in range(4))
        self.master_read_buffer_read = Array(Signal() for a in range(4))
        self.master_read_buffer_start = Signal()

        self.submodules.slave_fsm = slave_fsm = FSM(reset_state="Reset")

        self.sync += platform.request("user_led", 5).eq(~slave_fsm.ongoing("Idle"))

        slave_fsm.act("Reset",
                      #NextValue(self.led_display.value, 0x0000000000),
                      NextValue(sbus_oe_data, 0),
                      NextValue(sbus_oe_slave_in, 0),
                      NextValue(sbus_oe_master_in, 0),
                      NextValue(p_data, 0),
                      NextState("Start"),
                      NextValue(self.wishbone_master.we, 0),
                      NextValue(self.wishbone_master.cyc, 0),
                      NextValue(self.wishbone_master.stb, 0),
                      NextValue(self.wishbone_slave.ack, 0),
                      NextValue(self.wishbone_slave.err, 0),
                      NextValue(wishbone_master_timeout, 0),
                      NextValue(wishbone_slave_timeout, 0),
                      NextValue(sbus_slave_timeout, 0)
        )
        slave_fsm.act("Start",
                      #NextValue(self.led_display.value, 0x0FF0000000),
                      NextValue(sbus_oe_data, 0),
                      NextValue(sbus_oe_slave_in, 0),
                      NextValue(sbus_oe_master_in, 0),
                      NextValue(p_data, 0),
                      If((self.hold_reset == 0), NextState("Idle"))
        )
        slave_fsm.act("Idle",
                      If(((SBUS_3V3_SELs_i == 0) &
                          (SBUS_3V3_ASs_i == 0) &
                          (siz_is_word(SBUS_3V3_SIZ_i)) &
                          (SBUS_3V3_PPRD_i == 1)),
                         NextValue(sbus_oe_master_in, 1),
                         NextValue(sbus_last_pa, SBUS_3V3_PA_i),
                         NextValue(burst_counter, 0),
                         Case(SBUS_3V3_SIZ_i, {
                             SIZ_WORD: NextValue(burst_limit_m1, 0),
                             SIZ_BURST2: NextValue(burst_limit_m1, 1),
                             SIZ_BURST4: NextValue(burst_limit_m1, 3),
                             SIZ_BURST8: NextValue(burst_limit_m1, 7),
                             SIZ_BURST16: NextValue(burst_limit_m1, 15)}),
                         If(SBUS_3V3_PA_i[0:2] != 0,
                            NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                            NextValue(SBUS_3V3_ERRs_o, 1),
                            #NextValue(led0123, led0123 | LED_PARITY),
                            NextState("Slave_Error")
                         ).Elif(((SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == ROM_ADDR_PFX) |
                                 (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == WISHBONE_CSR_ADDR_PFX) |
                                 (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == USBOHCI_ADDR_PFX) |
                                 (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                NextValue(SBUS_3V3_ACKs_o, ACK_IDLE), # need to wait for data, don't ACK yet
                                NextValue(SBUS_3V3_ERRs_o, 1),
                                NextValue(sbus_wishbone_le, (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                If(self.wishbone_master.cyc == 0,
                                   NextValue(self.wishbone_master.cyc, 1),
                                   NextValue(self.wishbone_master.stb, 1),
                                   NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                                   NextValue(self.wishbone_master.we, 0),
                                   NextValue(self.wishbone_master.adr, Cat(SBUS_3V3_PA_i[2:28], Signal(4, reset = 0))),
                                   NextValue(wishbone_master_timeout, wishbone_default_timeout),
                                   NextValue(sbus_slave_timeout, sbus_default_timeout),
                                   #NextValue(self.led_display.value, 0x0000000000 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                                   NextState("Slave_Ack_Read_Reg_Burst_Wait_For_Data")
                                ).Else(
                                   NextValue(sbus_slave_timeout, sbus_default_timeout),
                                   NextState("Slave_Ack_Read_Reg_Burst_Wait_For_Wishbone")
                                )
                         ).Else(
                             #NextValue(self.led_display.value, 0x0000000020 | 0x0000000001),
                             NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                             NextValue(SBUS_3V3_ERRs_o, 1),
                             #NextValue(led0123, led0123 | LED_ADDRESS),
                             NextState("Slave_Error")
                         )
                      ).Elif(((SBUS_3V3_SELs_i == 0) &
                              (SBUS_3V3_ASs_i == 0) &
                              (SIZ_BYTE == SBUS_3V3_SIZ_i) &
                              (SBUS_3V3_PPRD_i == 1)),
                             NextValue(sbus_oe_master_in, 1),
                             NextValue(sbus_last_pa, SBUS_3V3_PA_i),
                             If(((SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == ROM_ADDR_PFX) |
                                 (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                NextValue(SBUS_3V3_ACKs_o, ACK_IDLE), # need to wait for data, don't ACK yet
                                NextValue(SBUS_3V3_ERRs_o, 1),
                                NextValue(sbus_wishbone_le, (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                If(self.wishbone_master.cyc == 0,
                                   NextValue(self.wishbone_master.cyc, 1),
                                   NextValue(self.wishbone_master.stb, 1),
                                   NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                                   NextValue(self.wishbone_master.we, 0),
                                   NextValue(self.wishbone_master.adr, Cat(SBUS_3V3_PA_i[2:28], Signal(4, reset = 0))),
                                   NextValue(wishbone_master_timeout, wishbone_default_timeout),
                                   NextValue(sbus_slave_timeout, sbus_default_timeout),
                                   #NextValue(self.led_display.value, 0x0000000000 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                                   NextState("Slave_Ack_Read_Reg_Byte_Wait_For_Data")
                                ).Else(
                                   NextValue(sbus_slave_timeout, sbus_default_timeout),
                                   NextState("Slave_Ack_Read_Reg_Byte_Wait_For_Wishbone")
                                )
                             ).Else(
                                 #NextValue(self.led_display.value, 0x0000000040 | 0x0000000001),
                                 NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                                 NextValue(SBUS_3V3_ERRs_o, 1),
                                 #NextValue(led0123, led0123 | LED_ADDRESS),
                                 NextState("Slave_Error")
                             )
                      ).Elif(((SBUS_3V3_SELs_i == 0) &
                              (SBUS_3V3_ASs_i == 0) &
                              (SIZ_HWORD == SBUS_3V3_SIZ_i) &
                              (SBUS_3V3_PPRD_i == 1)),
                         NextValue(sbus_oe_master_in, 1),
                         NextValue(sbus_last_pa, SBUS_3V3_PA_i),
                         If(SBUS_3V3_PA_i[0:1] != 0,
                            NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                            NextValue(SBUS_3V3_ERRs_o, 1),
                            #NextValue(led0123, led0123 | LED_PARITY),
                            NextState("Slave_Error")
                         ).Elif(((SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == ROM_ADDR_PFX) |
                                 (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                NextValue(SBUS_3V3_ACKs_o, ACK_IDLE), # need to wait for data, don't ACK yet
                                NextValue(SBUS_3V3_ERRs_o, 1),
                                NextValue(sbus_wishbone_le, (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                If(self.wishbone_master.cyc == 0,
                                   NextValue(self.wishbone_master.cyc, 1),
                                   NextValue(self.wishbone_master.stb, 1),
                                   NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                                   NextValue(self.wishbone_master.we, 0),
                                   NextValue(self.wishbone_master.adr, Cat(SBUS_3V3_PA_i[2:28], Signal(4, reset = 0))),
                                   NextValue(wishbone_master_timeout, wishbone_default_timeout),
                                   NextValue(sbus_slave_timeout, sbus_default_timeout),
                                   #NextValue(self.led_display.value, 0x0000000000 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                                   NextState("Slave_Ack_Read_Reg_HWord_Wait_For_Data")
                                ).Else(
                                   NextValue(sbus_slave_timeout, sbus_default_timeout),
                                   NextState("Slave_Ack_Read_Reg_HWord_Wait_For_Wishbone")
                                )
                         ).Else(
                             #NextValue(self.led_display.value, 0x0000000040 | 0x0000000001),
                             NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                             NextValue(SBUS_3V3_ERRs_o, 1),
                             #NextValue(led0123, led0123 | LED_ADDRESS),
                             NextState("Slave_Error")
                         )
                      ).Elif(((SBUS_3V3_SELs_i == 0) &
                              (SBUS_3V3_ASs_i == 0) &
                              (siz_is_word(SBUS_3V3_SIZ_i)) &
                              (SBUS_3V3_PPRD_i == 0)),
                             NextValue(sbus_oe_master_in, 1),
                             NextValue(sbus_last_pa, SBUS_3V3_PA_i),
                             NextValue(burst_counter, 0),
                             Case(SBUS_3V3_SIZ_i, {
                                 SIZ_WORD: NextValue(burst_limit_m1, 0),
                                 SIZ_BURST2: NextValue(burst_limit_m1, 1),
                                 SIZ_BURST4: NextValue(burst_limit_m1, 3),
                                 SIZ_BURST8: NextValue(burst_limit_m1, 7),
                                 SIZ_BURST16: NextValue(burst_limit_m1, 15)
                             }),
                             If(SBUS_3V3_PA_i[0:2] != 0,
                                NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                                NextValue(SBUS_3V3_ERRs_o, 1),
                                #NextValue(led0123, led0123 | LED_PARITY),
                                NextState("Slave_Error")
                             ).Elif(((SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == WISHBONE_CSR_ADDR_PFX) |
                                     (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == USBOHCI_ADDR_PFX) |
                                     (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                    NextValue(sbus_wishbone_le, (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                    If(~self.wishbone_master.cyc,
                                       NextValue(SBUS_3V3_ACKs_o, ACK_WORD),
                                       NextValue(SBUS_3V3_ERRs_o, 1),
                                       #NextValue(self.led_display.value, 0x0000000010 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                                       NextState("Slave_Ack_Reg_Write_Burst")
                                    ).Else(
                                        NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                                        NextValue(SBUS_3V3_ERRs_o, 1),
                                        NextValue(sbus_slave_timeout, sbus_default_timeout),
                                        NextState("Slave_Ack_Reg_Write_Burst_Wait_For_Wishbone")
                                    )
                             ).Else(
                                 #NextValue(self.led_display.value, 0x0000000060 | 0x0000000001),
                                 NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                                 NextValue(SBUS_3V3_ERRs_o, 1),
                                 #NextValue(led0123, led0123 | LED_ADDRESS),
                                 NextState("Slave_Error")
                             )
                      ).Elif(((SBUS_3V3_SELs_i == 0) &
                              (SBUS_3V3_ASs_i == 0) &
                              (SIZ_BYTE == SBUS_3V3_SIZ_i) &
                              (SBUS_3V3_PPRD_i == 0)),
                         NextValue(sbus_oe_master_in, 1),
                         NextValue(sbus_last_pa, SBUS_3V3_PA_i),
                         If((SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX),
                            NextValue(sbus_wishbone_le, (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                            If(~self.wishbone_master.cyc,
                                NextValue(SBUS_3V3_ACKs_o, ACK_BYTE),
                                NextValue(SBUS_3V3_ERRs_o, 1),
                                #NextValue(self.led_display.value, 0x0000000010 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                                NextState("Slave_Ack_Reg_Write_Byte")
                            ).Else(
                                NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                                NextValue(SBUS_3V3_ERRs_o, 1),
                                NextValue(sbus_slave_timeout, sbus_default_timeout),
                                NextState("Slave_Ack_Reg_Write_Byte_Wait_For_Wishbone")
                            )
                         ).Else(
                             #NextValue(self.led_display.value, 0x0000000060 | 0x0000000001),
                             NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                             NextValue(SBUS_3V3_ERRs_o, 1),
                             #NextValue(led0123, led0123 | LED_ADDRESS),
                             NextState("Slave_Error")
                         )
                      ).Elif(((SBUS_3V3_SELs_i == 0) &
                              (SBUS_3V3_ASs_i == 0) &
                              (SIZ_HWORD == SBUS_3V3_SIZ_i) &
                              (SBUS_3V3_PPRD_i == 0)),
                             NextValue(sbus_oe_master_in, 1),
                             NextValue(sbus_last_pa, SBUS_3V3_PA_i),
                             If(SBUS_3V3_PA_i[0:1] != 0,
                                NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                                NextValue(SBUS_3V3_ERRs_o, 1),
                                #NextValue(led0123, led0123 | LED_PARITY),
                                NextState("Slave_Error")
                             ).Elif((SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX),
                                    NextValue(sbus_wishbone_le, (SBUS_3V3_PA_i[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH] == SRAM_ADDR_PFX)),
                                    If(~self.wishbone_master.cyc,
                                       NextValue(SBUS_3V3_ACKs_o, ACK_HWORD),
                                       NextValue(SBUS_3V3_ERRs_o, 1),
                                       #NextValue(self.led_display.value, 0x0000000010 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                                       NextState("Slave_Ack_Reg_Write_HWord")
                                    ).Else(
                                        NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                                        NextValue(SBUS_3V3_ERRs_o, 1),
                                        NextValue(sbus_slave_timeout, sbus_default_timeout),
                                        NextState("Slave_Ack_Reg_Write_HWord_Wait_For_Wishbone")
                                    )
                             ).Else(
                                 #NextValue(self.led_display.value, 0x0000000060 | 0x0000000001),
                                 NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                                 NextValue(SBUS_3V3_ERRs_o, 1),
                                 #NextValue(led0123, led0123 | LED_ADDRESS),
                                 NextState("Slave_Error")
                             )
                      ).Elif(self.wishbone_slave.cyc &
                             self.wishbone_slave.stb &
                             ~self.wishbone_slave.ack &
                             ~self.wishbone_slave.err &
                             self.wishbone_slave.we &
                             (self.wishbone_slave.sel == 0) &
                             (wishbone_slave_timeout == 0),
                             ## sel == 0 so nothing to write, don't acquire the SBus
                             NextValue(self.wishbone_slave.ack, 1),
                             NextValue(wishbone_slave_timeout, wishbone_default_timeout),
                      ).Elif(SBUS_3V3_BGs_i &
                             self.wishbone_slave.cyc &
                             self.wishbone_slave.stb &
                             ~self.wishbone_slave.ack &
                             ~self.wishbone_slave.err &
                             self.wishbone_slave.we &
                             (sbus_master_throttle == 0) &
                             (wishbone_slave_timeout == 0),
                             NextValue(SBUS_3V3_BRs_o, 0)
                      ).Elif(~SBUS_3V3_BGs_i &
                             self.wishbone_slave.cyc &
                             self.wishbone_slave.stb &
                             ~self.wishbone_slave.ack &
                             ~self.wishbone_slave.err &
                             self.wishbone_slave.we,
                             NextValue(sbus_wishbone_le, 1), # checkme
                             NextValue(SBUS_3V3_BRs_o, 1), # relinquish the request
                             NextValue(sbus_oe_data, 1), ## output data (at least for @ during translation)
                             NextValue(sbus_oe_slave_in, 1), ## PPRD, SIZ becomes output
                             NextValue(sbus_oe_master_in, 0), ## ERRs, ACKs are input
                             NextValue(burst_counter, 0),
                             NextValue(burst_limit_m1, 0), ## only single word for now
                             NextValue(master_addr, self.wishbone_slave.adr),
                             NextValue(master_data, Cat(self.wishbone_slave.dat_w[24:32], ## LE
                                                        self.wishbone_slave.dat_w[16:24],
                                                        self.wishbone_slave.dat_w[ 8:16],
                                                        self.wishbone_slave.dat_w[ 0: 8])),
                             Case(self.wishbone_slave.sel, {
                                 0xf: [NextValue(burst_counter, 0),
                                      NextValue(burst_limit_m1, 0), ## only single word for now
                                      NextValue(master_size, SIZ_WORD),
                                      NextValue(SBUS_3V3_SIZ_o, SIZ_WORD),
                                      NextValue(SBUS_3V3_D_o, Cat(Signal(2, reset = 0), self.wishbone_slave.adr)),
                                 ],
                                 0x1: [NextValue(master_idx, 3),
                                     NextValue(master_size, SIZ_BYTE),
                                     NextValue(SBUS_3V3_SIZ_o, SIZ_BYTE),
                                     NextValue(SBUS_3V3_D_o, Cat(Signal(2, reset = 0), self.wishbone_slave.adr)),
                                 ],
                                 0x2: [NextValue(master_idx, 2),
                                     NextValue(master_size, SIZ_BYTE),
                                     NextValue(SBUS_3V3_SIZ_o, SIZ_BYTE),
                                     NextValue(SBUS_3V3_D_o, Cat(Signal(2, reset = 1), self.wishbone_slave.adr)),
                                 ],
                                 0x4: [NextValue(master_idx, 1),
                                     NextValue(master_size, SIZ_BYTE),
                                     NextValue(SBUS_3V3_SIZ_o, SIZ_BYTE),
                                     NextValue(SBUS_3V3_D_o, Cat(Signal(2, reset = 2), self.wishbone_slave.adr)),
                                 ],
                                 0x8: [NextValue(master_idx, 0),
                                     NextValue(master_size, SIZ_BYTE),
                                     NextValue(SBUS_3V3_SIZ_o, SIZ_BYTE),
                                     NextValue(SBUS_3V3_D_o, Cat(Signal(2, reset = 3), self.wishbone_slave.adr)),
                                 ],
                                 0x3: [NextValue(master_idx, 2),
                                     NextValue(master_size, SIZ_HWORD),
                                     NextValue(SBUS_3V3_SIZ_o, SIZ_HWORD),
                                     NextValue(SBUS_3V3_D_o, Cat(Signal(2, reset = 0), self.wishbone_slave.adr)),
                                 ],
                                 0xc: [NextValue(master_idx, 0),
                                     NextValue(master_size, SIZ_HWORD),
                                     NextValue(SBUS_3V3_SIZ_o, SIZ_HWORD),
                                     NextValue(SBUS_3V3_D_o, Cat(Signal(2, reset = 2), self.wishbone_slave.adr)),
                                 ],
                                 "default":[NextValue(burst_counter, 0), # FIXME if it happens!
                                            NextValue(burst_limit_m1, 0), ## only single word for now
                                            NextValue(master_size, SIZ_WORD),
                                            NextValue(SBUS_3V3_SIZ_o, SIZ_WORD),
                                            NextValue(led0123, self.wishbone_slave.sel)
                                 ]
                             }),
#                             NextValue(master_data, self.wishbone_slave.dat_w),
                             NextValue(self.wishbone_slave.ack, 1),
                             NextValue(wishbone_slave_timeout, wishbone_default_timeout),
                             NextValue(SBUS_3V3_PPRD_o, 0),
                             NextValue(master_we, 1),
                             #NextValue(self.led_display.value, 0x0000000010 | Cat(Signal(8, reset = 0x00), self.wishbone_slave.adr)),
                             #NextValue(self.led_display.value, Cat(Signal(8, reset = LED_M_WRITE), Signal(2, reset = 0), self.wishbone_slave.adr)), 
                             NextState("Master_Translation")
                      ).Elif(SBUS_3V3_BGs_i &
                             self.master_read_buffer_start &
                             (sbus_master_throttle == 0) &
                             (wishbone_slave_timeout == 0),
                             NextValue(SBUS_3V3_BRs_o, 0)
                      ).Elif(~SBUS_3V3_BGs_i &
                             self.master_read_buffer_start,
                             NextValue(sbus_wishbone_le, 1), # checkme
                             NextValue(SBUS_3V3_BRs_o, 1), # relinquish the request
                             NextValue(sbus_oe_data, 1), ## output data (at least for @ during translation)
                             NextValue(sbus_oe_slave_in, 1), ## PPRD, SIZ becomes output
                             NextValue(sbus_oe_master_in, 0), ## ERRs, ACKs are input
                             NextValue(burst_counter, 0),
                             NextValue(burst_limit_m1, 3), ## only quadword word for now
                             NextValue(SBUS_3V3_D_o, Cat(Signal(4, reset = 0), self.master_read_buffer_addr)),
                             NextValue(SBUS_3V3_PPRD_o, 1),
                             NextValue(SBUS_3V3_SIZ_o, SIZ_BURST4),
                             NextValue(master_we, 0),
                             #NextValue(self.led_display.value, 0x0000000000 | Cat(Signal(8, reset = 0x00), self.wishbone_slave.adr)),
                             #NextValue(self.led_display.value, Cat(Signal(8, reset = LED_M_READ), Signal(2, reset = 0), self.master_read_buffer_addr)), 
                             NextState("Master_Translation")
                      ).Elif(((SBUS_3V3_SELs_i == 0) &
                              (SBUS_3V3_ASs_i == 0)),
                             NextValue(sbus_oe_master_in, 1),
                             NextValue(SBUS_3V3_ACKs_o, ACK_ERR),
                             NextValue(SBUS_3V3_ERRs_o, 1),
                             #NextValue(self.led_display.value, 0x000000000F | Cat(Signal(8, reset = 0x00), SBUS_3V3_PA_i, SBUS_3V3_SIZ_i, SBUS_3V3_PPRD_i)),
                             #NextValue(led0123, led0123 | LED_UNKNOWNREQ),
                             NextState("Slave_Error")
                      ).Elif(~SBUS_3V3_BGs_i,
                             ### ouch we got the bus but nothing more to do ?!?
                             NextValue(SBUS_3V3_BRs_o, 1),
                      ).Else(
                          # FIXME: handle error
                      )
        )
        # ##### SLAVE READ #####
        # ## BURST (1->16 words) ##
        slave_fsm.act("Slave_Do_Read",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x04), self.led_display.value[8:40])),
                      NextValue(sbus_oe_data, 0),
                      NextValue(sbus_oe_slave_in, 0),
                      NextValue(sbus_oe_master_in, 0),
                      If(((SBUS_3V3_ASs_i == 1) | ((SBUS_3V3_ASs_i == 0) & (SBUS_3V3_SELs_i == 1))),
                         NextState("Idle")
                      )
        )
        slave_fsm.act("Slave_Ack_Read_Reg_Burst",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x05), self.led_display.value[8:40])),
                      NextValue(sbus_oe_data, 1),
                      NextValue(SBUS_3V3_D_o, p_data),
                      If((burst_counter == burst_limit_m1),
                         NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                         NextState("Slave_Do_Read")
                      ).Else(
                          NextValue(burst_counter, burst_counter + 1),
                          NextValue(self.wishbone_master.cyc, 1),
                          NextValue(self.wishbone_master.stb, 1),
                          NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                          NextValue(self.wishbone_master.we, 0),
                          NextValue(wishbone_master_timeout, wishbone_default_timeout),
                          NextValue(sbus_slave_timeout, sbus_default_timeout),
                          NextValue(self.wishbone_master.adr, Cat(index_with_wrap(burst_counter+1, burst_limit_m1, sbus_last_pa[ADDR_PHYS_LOW+2:ADDR_PHYS_LOW+6]), # 4 bits, adr FIXME
                                                                  sbus_last_pa[ADDR_PHYS_LOW+6:ADDR_PFX_LOW], # 10 bits, adr
                                                                  sbus_last_pa[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH], # 12 bits, adr
                                                                  Signal(4, reset = 0))),
                          NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                          NextState("Slave_Ack_Read_Reg_Burst_Wait_For_Data")
                      )
        )
        slave_fsm.act("Slave_Ack_Read_Reg_Burst_Wait_For_Data",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x06), self.led_display.value[8:40])),
                      If(self.wishbone_master.ack,
                         Case(sbus_wishbone_le, {
                             0: NextValue(p_data,self.wishbone_master.dat_r),
                             1: NextValue(p_data, Cat(self.wishbone_master.dat_r[24:32],
                                                      self.wishbone_master.dat_r[16:24],
                                                      self.wishbone_master.dat_r[ 8:16],
                                                      self.wishbone_master.dat_r[ 0: 8]))
                         }),
                         NextValue(self.wishbone_master.cyc, 0),
                         NextValue(self.wishbone_master.stb, 0),
                         NextValue(wishbone_master_timeout, 0),
                         NextValue(sbus_slave_timeout, 0),
                         NextValue(SBUS_3V3_ACKs_o, ACK_WORD),
                         NextState("Slave_Ack_Read_Reg_Burst")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(self.wishbone_master.cyc, 0), ## abort transaction
                             NextValue(self.wishbone_master.stb, 0),
                             NextValue(wishbone_master_timeout, 0),
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN),
                             #NextValue(led0123, LED_RERUN | LED_RERUN_WORD | LED_RERUN_LATE),
                             NextState("Slave_Error")
                      )
        )
        slave_fsm.act("Slave_Ack_Read_Reg_Burst_Wait_For_Wishbone",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x68), self.led_display.value[8:40])),
                      If(self.wishbone_master.cyc == 0,
                         NextValue(self.wishbone_master.cyc, 1),
                         NextValue(self.wishbone_master.stb, 1),
                         NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                         NextValue(self.wishbone_master.we, 0),
                         NextValue(self.wishbone_master.adr, Cat(sbus_last_pa[2:28], Signal(4, reset = 0))),
                         NextValue(wishbone_master_timeout, wishbone_default_timeout),
                         NextValue(sbus_slave_timeout, sbus_slave_timeout),
                         #NextValue(self.led_display.value, 0x0000000000 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                         NextState("Slave_Ack_Read_Reg_Burst_Wait_For_Data")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN), 
                             #NextValue(led0123, LED_RERUN | LED_RERUN_WORD),
                             NextState("Slave_Error")
                      )
        )
        # ## HWORD
        slave_fsm.act("Slave_Ack_Read_Reg_HWord",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x05), self.led_display.value[8:40])),
                      NextValue(sbus_oe_data, 1),
                      NextValue(SBUS_3V3_D_o, p_data),
                      NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                      NextState("Slave_Do_Read")
        )
        slave_fsm.act("Slave_Ack_Read_Reg_HWord_Wait_For_Data",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x06), self.led_display.value[8:40])),
                      If(self.wishbone_master.ack,
                         Case(sbus_wishbone_le, {
                             0: Case(sbus_last_pa[ADDR_PHYS_LOW+1:ADDR_PHYS_LOW+2], {
                                 0: NextValue(p_data, Cat(Signal(16, reset = 0),
                                                          self.wishbone_master.dat_r[16:32])),
                                 1: NextValue(p_data, Cat(Signal(16, reset = 0),
                                                          self.wishbone_master.dat_r[ 0:16])),
                             }),
                             1: Case(sbus_last_pa[ADDR_PHYS_LOW+1:ADDR_PHYS_LOW+2], {
                                 1: NextValue(p_data, Cat(Signal(16, reset = 0),
                                                          self.wishbone_master.dat_r[24:32],
                                                          self.wishbone_master.dat_r[16:24])),
                                 0: NextValue(p_data, Cat(Signal(16, reset = 0),
                                                          self.wishbone_master.dat_r[ 8:16],
                                                          self.wishbone_master.dat_r[ 0: 8])),
                             })
                         }),
                         NextValue(self.wishbone_master.cyc, 0),
                         NextValue(self.wishbone_master.stb, 0),
                         NextValue(wishbone_master_timeout, 0),
                         NextValue(sbus_slave_timeout, 0),
                         NextValue(SBUS_3V3_ACKs_o, ACK_HWORD),
                         NextState("Slave_Ack_Read_Reg_HWord")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(self.wishbone_master.cyc, 0), ## abort transaction
                             NextValue(self.wishbone_master.stb, 0),
                             NextValue(wishbone_master_timeout, 0),
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN), 
                             #NextValue(led0123, LED_RERUN | LED_RERUN_LATE),
                             NextState("Slave_Error")
                      )
        )
        slave_fsm.act("Slave_Ack_Read_Reg_HWord_Wait_For_Wishbone",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x68), self.led_display.value[8:40])),
                      If(self.wishbone_master.cyc == 0,
                         NextValue(self.wishbone_master.cyc, 1),
                         NextValue(self.wishbone_master.stb, 1),
                         NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                         NextValue(self.wishbone_master.we, 0),
                         NextValue(self.wishbone_master.adr, Cat(sbus_last_pa[2:28], Signal(4, reset = 0))),
                         NextValue(wishbone_master_timeout, wishbone_default_timeout),
                         NextValue(sbus_slave_timeout, sbus_slave_timeout),
                         #NextValue(self.led_display.value, 0x0000000000 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                         NextState("Slave_Ack_Read_Reg_HWord_Wait_For_Data")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN), 
                             #NextValue(led0123, LED_RERUN),
                             NextState("Slave_Error")
                      )
        )
        # ## BYTE
        slave_fsm.act("Slave_Ack_Read_Reg_Byte",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x05), self.led_display.value[8:40])),
                      NextValue(sbus_oe_data, 1),
                      NextValue(SBUS_3V3_D_o, p_data),
                      NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                      NextState("Slave_Do_Read")
        )
        slave_fsm.act("Slave_Ack_Read_Reg_Byte_Wait_For_Data",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x06), self.led_display.value[8:40])),
                      If(self.wishbone_master.ack,
                         Case(sbus_wishbone_le, {
                             0: Case(sbus_last_pa[ADDR_PHYS_LOW:ADDR_PHYS_LOW+2], {
                                 0: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[24:32])),
                                 1: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[16:24])),
                                 2: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[ 8:16])),
                                 3: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[ 0: 8])),
                             }),
                             1: Case(sbus_last_pa[ADDR_PHYS_LOW:ADDR_PHYS_LOW+2], {
                                 3: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[24:32])),
                                 2: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[16:24])),
                                 1: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[ 8:16])),
                                 0: NextValue(p_data, Cat(Signal(24, reset = 0), self.wishbone_master.dat_r[ 0: 8])),
                         })
                         }),
                         NextValue(self.wishbone_master.cyc, 0),
                         NextValue(self.wishbone_master.stb, 0),
                         NextValue(wishbone_master_timeout, 0),
                         NextValue(sbus_slave_timeout, 0),
                         NextValue(SBUS_3V3_ACKs_o, ACK_BYTE),
                         NextState("Slave_Ack_Read_Reg_Byte")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(self.wishbone_master.cyc, 0), ## abort transaction
                             NextValue(self.wishbone_master.stb, 0),
                             NextValue(wishbone_master_timeout, 0),
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN), 
                             #NextValue(led0123, LED_RERUN | LED_RERUN_LATE),
                             NextState("Slave_Error")
                      )
        )
        slave_fsm.act("Slave_Ack_Read_Reg_Byte_Wait_For_Wishbone",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x68), self.led_display.value[8:40])),
                      If(self.wishbone_master.cyc == 0,
                         NextValue(self.wishbone_master.cyc, 1),
                         NextValue(self.wishbone_master.stb, 1),
                         NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                         NextValue(self.wishbone_master.we, 0),
                         NextValue(self.wishbone_master.adr, Cat(sbus_last_pa[2:28], Signal(4, reset = 0))),
                         NextValue(wishbone_master_timeout, wishbone_default_timeout),
                         NextValue(sbus_slave_timeout, sbus_slave_timeout),
                         #NextValue(self.led_display.value, 0x0000000000 | Cat(Signal(8, reset = 0), SBUS_3V3_PA_i, Signal(4, reset = 0))),
                         NextState("Slave_Ack_Read_Reg_Byte_Wait_For_Data")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN), 
                             #NextValue(led0123, LED_RERUN),
                             NextState("Slave_Error")
                      )
        )
        # ##### SLAVE WRITE #####
        # ## BURST (1->16 words) ##
        slave_fsm.act("Slave_Ack_Reg_Write_Burst",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x07), self.led_display.value[8:40])),
                      NextValue(self.wishbone_master.cyc, 1),
                      NextValue(self.wishbone_master.stb, 1),
                      NextValue(self.wishbone_master.sel, 2**len(self.wishbone_master.sel)-1),
                      NextValue(self.wishbone_master.adr, Cat(index_with_wrap(burst_counter, burst_limit_m1, sbus_last_pa[ADDR_PHYS_LOW+2:ADDR_PHYS_LOW+6]), # 4 bits, adr FIXME
                                                              sbus_last_pa[ADDR_PHYS_LOW+6:ADDR_PFX_LOW], # 10 bits, adr
                                                              sbus_last_pa[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH], # 12 bits, adr
                                                              Signal(4, reset = 0))),
                      Case(sbus_wishbone_le, {
                          0: NextValue(self.wishbone_master.dat_w, Cat(SBUS_3V3_D_i)),
                          1: NextValue(self.wishbone_master.dat_w, Cat(SBUS_3V3_D_i[24:32],
                                                                       SBUS_3V3_D_i[16:24],
                                                                       SBUS_3V3_D_i[ 8:16],
                                                                       SBUS_3V3_D_i[ 0: 8]))
                      }),
                      NextValue(self.wishbone_master.we, 1),
                      NextValue(wishbone_master_timeout, wishbone_default_timeout),
                      If((burst_counter == burst_limit_m1),
                         NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                         NextState("Slave_Ack_Reg_Write_Final")
                      ).Else(
                          NextValue(SBUS_3V3_ACKs_o, ACK_WORD),
                          NextValue(burst_counter, burst_counter + 1)
                      )
        )
        slave_fsm.act("Slave_Ack_Reg_Write_Final",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x08), self.led_display.value[8:40])),
                      NextValue(sbus_oe_data, 0),
                      NextValue(sbus_oe_slave_in, 0),
                      NextValue(sbus_oe_master_in, 0),
                      If(((SBUS_3V3_ASs_i == 1) | ((SBUS_3V3_ASs_i == 0) & (SBUS_3V3_SELs_i == 1))),
                         NextState("Idle")
                      )
        )
        slave_fsm.act("Slave_Ack_Reg_Write_Burst_Wait_For_Wishbone",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x68), self.led_display.value[8:40])),
                      If(self.wishbone_master.cyc == 0,
                         NextValue(sbus_slave_timeout, 0),
                         NextValue(SBUS_3V3_ACKs_o, ACK_WORD),
                         NextState("Slave_Ack_Reg_Write_Burst")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN),
                             #NextValue(self.led_display.value, Cat(Signal(8, reset = LED_RERUN | LED_RERUN_WRITE | LED_RERUN_WORD), sbus_last_pa, Signal(4, reset = 0))),
                             #NextValue(led0123, LED_RERUN | LED_RERUN_WRITE | LED_RERUN_WORD),
                             NextState("Slave_Error")
                      )
        )
        # ## HWORD
        slave_fsm.act("Slave_Ack_Reg_Write_HWord",
                      NextValue(self.wishbone_master.cyc, 1),
                      NextValue(self.wishbone_master.stb, 1),
                      Case(sbus_wishbone_le, {
                          0: Case(sbus_last_pa[ADDR_PHYS_LOW+1:ADDR_PHYS_LOW+2], {
                              0: NextValue(self.wishbone_master.sel, 0xc),
                              1: NextValue(self.wishbone_master.sel, 0x3),
                          }),
                          1: Case(sbus_last_pa[ADDR_PHYS_LOW+1:ADDR_PHYS_LOW+2], {
                              1: NextValue(self.wishbone_master.sel, 0xc),
                              0: NextValue(self.wishbone_master.sel, 0x3),
                          }),
                      }),
                      NextValue(self.wishbone_master.adr, Cat(sbus_last_pa[ADDR_PHYS_LOW+2:ADDR_PHYS_LOW+6], # 4 bits, adr FIXME
                                                              sbus_last_pa[ADDR_PHYS_LOW+6:ADDR_PFX_LOW], # 10 bits, adr
                                                              sbus_last_pa[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH], # 12 bits, adr
                                                              Signal(4, reset = 0))),
                      Case(sbus_wishbone_le, {
                          0: NextValue(self.wishbone_master.dat_w, Cat(SBUS_3V3_D_i[16:32],
                                                                       SBUS_3V3_D_i[16:32])),
                          1: NextValue(self.wishbone_master.dat_w, Cat(SBUS_3V3_D_i[24:32],
                                                                       SBUS_3V3_D_i[16:24],
                                                                       SBUS_3V3_D_i[24:32],
                                                                       SBUS_3V3_D_i[16:24])),
                      }),
                      NextValue(self.wishbone_master.we, 1),
                      NextValue(wishbone_master_timeout, wishbone_default_timeout),
                      NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                      NextState("Slave_Ack_Reg_Write_Final")
        )
        slave_fsm.act("Slave_Ack_Reg_Write_HWord_Wait_For_Wishbone",
                      If(self.wishbone_master.cyc == 0,
                         NextValue(sbus_slave_timeout, 0),
                         NextValue(SBUS_3V3_ACKs_o, ACK_HWORD),
                         NextState("Slave_Ack_Reg_Write_HWord")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN), 
                             #NextValue(led0123, LED_RERUN | LED_RERUN_WRITE),
                             NextState("Slave_Error")
                      )
        )
        # ## BYTE
        slave_fsm.act("Slave_Ack_Reg_Write_Byte",
                      NextValue(self.wishbone_master.cyc, 1),
                      NextValue(self.wishbone_master.stb, 1),
                      Case(sbus_wishbone_le, {
                          0: Case(sbus_last_pa[ADDR_PHYS_LOW:ADDR_PHYS_LOW+2], {
                              0: NextValue(self.wishbone_master.sel, 0x8),
                              1: NextValue(self.wishbone_master.sel, 0x4),
                              2: NextValue(self.wishbone_master.sel, 0x2),
                              3: NextValue(self.wishbone_master.sel, 0x1),
                          }),
                          1: Case(sbus_last_pa[ADDR_PHYS_LOW:ADDR_PHYS_LOW+2], {
                              3: NextValue(self.wishbone_master.sel, 0x8),
                              2: NextValue(self.wishbone_master.sel, 0x4),
                              1: NextValue(self.wishbone_master.sel, 0x2),
                              0: NextValue(self.wishbone_master.sel, 0x1),
                          }),
                      }),
                      NextValue(self.wishbone_master.adr, Cat(sbus_last_pa[ADDR_PHYS_LOW+2:ADDR_PHYS_LOW+6], # 4 bits, adr FIXME
                                                              sbus_last_pa[ADDR_PHYS_LOW+6:ADDR_PFX_LOW], # 10 bits, adr
                                                              sbus_last_pa[ADDR_PFX_LOW:ADDR_PFX_LOW+ADDR_PFX_LENGTH], # 12 bits, adr
                                                              Signal(4, reset = 0))),
                      NextValue(self.wishbone_master.dat_w, Cat(SBUS_3V3_D_i[24:32], # LE/BE identical
                                                                SBUS_3V3_D_i[24:32],
                                                                SBUS_3V3_D_i[24:32],
                                                                SBUS_3V3_D_i[24:32])),
                      NextValue(self.wishbone_master.we, 1),
                      NextValue(wishbone_master_timeout, wishbone_default_timeout),
                      NextValue(SBUS_3V3_ACKs_o, ACK_IDLE),
                      NextState("Slave_Ack_Reg_Write_Final")
        )
        slave_fsm.act("Slave_Ack_Reg_Write_Byte_Wait_For_Wishbone",
                      If(self.wishbone_master.cyc == 0,
                         NextValue(sbus_slave_timeout, 0),
                         NextValue(SBUS_3V3_ACKs_o, ACK_BYTE),
                         NextState("Slave_Ack_Reg_Write_Byte")
                      ).Elif(sbus_slave_timeout == 0, ### this is taking too long
                             NextValue(SBUS_3V3_ACKs_o, ACK_RERUN), 
                             #NextValue(led0123, LED_RERUN | LED_RERUN_WRITE),
                             NextState("Slave_Error")
                      )
        )
        # ##### SLAVE ERROR #####
        slave_fsm.act("Slave_Error",
                      NextValue(SBUS_3V3_ACKs_o, ACK_IDLE), 
                      #NextValue(self.led_display.value, 0x0000000080 | self.led_display.value),
                      If(((SBUS_3V3_ASs_i == 1) | ((SBUS_3V3_ASs_i == 0) & (SBUS_3V3_SELs_i == 1))),
                         NextValue(sbus_oe_data, 0),
                         NextValue(sbus_oe_slave_in, 0),
                         NextValue(sbus_oe_master_in, 0),
                         NextState("Idle")
                      )
        )
        # ##### MASTER #####
        slave_fsm.act("Master_Translation",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x09), self.led_display.value[8:40])),
                      If(master_we,
                         NextValue(sbus_oe_data, 1),
                         Case(master_size, {
                             SIZ_WORD: NextValue(SBUS_3V3_D_o, master_data),
                             SIZ_BYTE: Case(master_idx, {
                                 0: NextValue(SBUS_3V3_D_o, Cat(master_data[ 0: 8],
                                                                master_data[ 0: 8],
                                                                master_data[ 0: 8],
                                                                master_data[ 0: 8],)),
                                 1: NextValue(SBUS_3V3_D_o, Cat(master_data[ 8:16],
                                                                master_data[ 8:16],
                                                                master_data[ 8:16],
                                                                master_data[ 8:16],)),
                                 2: NextValue(SBUS_3V3_D_o, Cat(master_data[16:24],
                                                                master_data[16:24],
                                                                master_data[16:24],
                                                                master_data[16:24],)),
                                 3: NextValue(SBUS_3V3_D_o, Cat(master_data[24:32],
                                                                master_data[24:32],
                                                                master_data[24:32],
                                                                master_data[24:32],)),
                                 }),
                             SIZ_HWORD: Case(master_idx, {
                                 0: NextValue(SBUS_3V3_D_o, Cat(master_data[ 0:16],
                                                                master_data[ 0:16],)),
                                 2: NextValue(SBUS_3V3_D_o, Cat(master_data[16:32],
                                                                master_data[16:32],)),
                                 })
                             })
                      ).Else(
                         NextValue(sbus_oe_data, 0)
                      ),
                      Case(SBUS_3V3_ACKs_i, {
                          ACK_ERR: ## ouch
                          [NextValue(wishbone_slave_timeout, wishbone_default_timeout),
                           NextValue(self.wishbone_slave.err, 1),
                           NextValue(sbus_oe_data, 0),
                           NextValue(sbus_oe_slave_in, 0),
                           NextValue(sbus_oe_master_in, 0),
                           NextState("Idle")],
                          ACK_RERUN: ### dunno how to handle that yet,
                          [NextValue(wishbone_slave_timeout, wishbone_default_timeout),
                           NextValue(self.wishbone_slave.err, 1),
                           NextValue(sbus_oe_data, 0),
                           NextValue(sbus_oe_slave_in, 0),
                           NextValue(sbus_oe_master_in, 0),
                           NextState("Idle")],
                          ACK_IDLE:
                          [If(master_we,
                              NextState("Master_Write")
                              ## FIXME: in burst mode, should update master_data with the next value
                              ## FIXME: we don't do burst mode yet
                          ).Else(
                              NextState("Master_Read")
                          )],
                          "default":
                          [If(SBUS_3V3_BGs_i, ## oups, we lost our bus access without error ?!?
                              NextValue(sbus_oe_data, 0),
                              NextValue(sbus_oe_slave_in, 0),
                              NextValue(sbus_oe_master_in, 0),
                              NextState("Idle")
                          )],
                      })
        )
        slave_fsm.act("Master_Read",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x0a), self.led_display.value[8:40])),
                      Case(SBUS_3V3_ACKs_i, {
                          ACK_WORD:
                          [NextState("Master_Read_Ack")
                          ],
                          ACK_IDLE:
                          [NextState("Master_Read") ## redundant
                          ],
                          ACK_RERUN: ### burst not handled
                          [NextValue(wishbone_slave_timeout, wishbone_default_timeout),
                           NextValue(self.wishbone_slave.err, 1),
                           NextValue(sbus_oe_data, 0),
                           NextValue(sbus_oe_slave_in, 0),
                           NextValue(sbus_oe_master_in, 0),
                           NextState("Idle")
                          ],
                          "default": ## ACK_ERRS or other ### burst not handled
                          [NextValue(wishbone_slave_timeout, wishbone_default_timeout),
                           NextValue(self.wishbone_slave.err, 1),
                           NextValue(sbus_oe_data, 0),
                           NextValue(sbus_oe_slave_in, 0),
                           NextValue(sbus_oe_master_in, 0),
                           NextState("Idle")
                          ],
                      })
        )
        slave_fsm.act("Master_Read_Ack",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x0b), self.led_display.value[8:40])),
                      NextValue(self.master_read_buffer_data[burst_counter[0:2]], SBUS_3V3_D_i),
                      NextValue(self.master_read_buffer_done[burst_counter[0:2]], 1),
                      NextValue(burst_counter, burst_counter + 1),
                      If(burst_counter == burst_limit_m1,
                         NextValue(self.master_read_buffer_start, 0),
                         NextState("Master_Read_Finish")
                      ).Else(
                          Case(SBUS_3V3_ACKs_i, {
                              ACK_WORD: NextState("Master_Read_Ack"), ## redundant
                              ACK_IDLE: NextState("Master_Read"),
                              ACK_RERUN: ### dunno how to handle that yet
                              [NextValue(sbus_oe_data, 0),
                               NextValue(sbus_oe_slave_in, 0),
                               NextValue(sbus_oe_master_in, 0),
                               NextState("Idle")
                              ],
                              "default":
                              [NextValue(sbus_oe_data, 0),
                               NextValue(sbus_oe_slave_in, 0),
                               NextValue(sbus_oe_master_in, 0),
                               NextState("Idle")
                              ],
                          }),
                      )
        )
        slave_fsm.act("Master_Read_Finish", ## missing the handling of late error
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x0c), self.led_display.value[8:40])),
                      NextValue(sbus_oe_data, 0),
                      NextValue(sbus_oe_slave_in, 0),
                      NextValue(sbus_oe_master_in, 0),
                      NextState("Idle")
        )
        slave_fsm.act("Master_Write",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x0d), self.led_display.value[8:40])),
                      Case(SBUS_3V3_ACKs_i, {
                          ACK_WORD: # FIXME: check againt master_size ?
                          [If(burst_counter == burst_limit_m1,
                              NextState("Master_Write_Final"),
                          ).Else(
                              NextValue(SBUS_3V3_D_o, master_data), ## FIXME: we're not updating master_data for burst mode yet
                              NextValue(burst_counter, burst_counter + 1),
                          )],
                          ACK_BYTE: # FIXME: check againt master_size ?
                          [NextState("Master_Write_Final"),
                          ],
                          ACK_HWORD: # FIXME: check againt master_size ?
                          [NextState("Master_Write_Final"),
                          ],
                          ACK_IDLE:
                          [NextState("Master_Write") ## redundant
                          ],
                          ACK_RERUN: ### dunno how to handle that yet
                          [NextValue(sbus_oe_data, 0),
                           NextValue(sbus_oe_slave_in, 0),
                           NextValue(sbus_oe_master_in, 0),
                           NextState("Idle")
                          ],
                          "default": ## ACK_ERRS or other
                          [NextValue(sbus_oe_data, 0),
                           NextValue(sbus_oe_slave_in, 0),
                           NextValue(sbus_oe_master_in, 0),
                           NextState("Idle")
                          ],
                      })
        )
        slave_fsm.act("Master_Write_Final",
                      #NextValue(self.led_display.value, Cat(Signal(8, reset = 0x0e), self.led_display.value[8:40])),
                      NextValue(sbus_oe_data, 0),
                      NextValue(sbus_oe_slave_in, 0),
                      NextValue(sbus_oe_master_in, 0),
                      NextValue(sbus_master_throttle, 7),
                      NextState("Idle")
        )
        # ##### FINISHED #####


        # ##### FSMs to finish wishbone transactions asynchronously
        
        self.submodules.wishbone_master_wait_fsm = wishbone_master_wait_fsm = FSM(reset_state="Reset")
        wishbone_master_wait_fsm.act("Reset",
                                     NextState("Idle")
        )
        wishbone_master_wait_fsm.act("Idle",
                        If(wishbone_master_timeout != 0,
                            NextValue(wishbone_master_timeout, wishbone_master_timeout -1)
                        ),
                        If(self.wishbone_master.cyc & self.wishbone_master.stb & self.wishbone_master.we,
                           If(self.wishbone_master.ack,# | (wishbone_master_timeout == 0),
                              #If(~self.wishbone_master.ack,
                              #    NextValue(led7, 1)
                              #),
                              NextValue(self.wishbone_master.cyc, 0),
                              NextValue(self.wishbone_master.stb, 0),
                              NextValue(self.wishbone_master.we, 0),
                              NextValue(wishbone_master_timeout, 0)
                           )
                        )
        )

        
        self.submodules.wishbone_slave_wait_fsm = wishbone_slave_wait_fsm = FSM(reset_state="Reset")
        wishbone_slave_wait_fsm.act("Reset",
                                    NextState("Idle")
        )
        wishbone_slave_wait_fsm.act("Idle",
                        If(wishbone_slave_timeout != 0,
                            NextValue(wishbone_slave_timeout, wishbone_slave_timeout -1)
                        ),
                        If(self.wishbone_slave.ack & self.wishbone_slave.we,
                           If((~self.wishbone_slave.stb), # | (wishbone_slave_timeout == 0), #~self.wishbone_slave.cyc & 
                              NextValue(self.wishbone_slave.ack, 0),
                              NextValue(wishbone_slave_timeout, 0)
                           )
                        ),
                        If(self.wishbone_slave.ack & ~self.wishbone_slave.we,
                           If((~self.wishbone_slave.stb), # | (wishbone_slave_timeout == 0), #~self.wishbone_slave.cyc & 
                              NextValue(self.wishbone_slave.ack, 0),
                              NextValue(wishbone_slave_timeout, 0)
                           )
                        ),
                        If(self.wishbone_slave.err,
                           If((~self.wishbone_slave.stb), # | (wishbone_slave_timeout == 0), #~self.wishbone_slave.cyc & 
                              NextValue(self.wishbone_slave.err, 0),
                              NextValue(wishbone_slave_timeout, 0)
                           )
                        )
        )

        self.submodules.sbus_slave_wait_fsm = sbus_slave_wait_fsm = FSM(reset_state="Reset")
        sbus_slave_wait_fsm.act("Reset",
                        NextState("Idle")
        )
        sbus_slave_wait_fsm.act("Idle",
                        If(sbus_slave_timeout != 0,
                            NextValue(sbus_slave_timeout, sbus_slave_timeout -1)
                        ),
        )

        # ##### FIXME: debug only?
        self.submodules.sbus_master_throttle_fsm = sbus_master_throttle_fsm = FSM(reset_state="Reset")
        sbus_master_throttle_fsm.act("Reset",
                        NextState("Idle")
        )
        sbus_master_throttle_fsm.act("Idle",
                        If(sbus_master_throttle != 0,
                            NextValue(sbus_master_throttle, sbus_master_throttle -1)
                        ),
        )

        # ##### Slave read buffering FSM ####
        last_word_idx = Signal(2)
        self.submodules.wishbone_slave_buffering_fsm = wishbone_slave_buffering_fsm = FSM(reset_state="Reset")
        #self.sync += led4.eq(self.master_read_buffer_start)
        wishbone_slave_buffering_fsm.act("Reset",
                                         NextState("Idle")
        )
        wishbone_slave_buffering_fsm.act("Idle",
                                         If(self.wishbone_slave.cyc &
                                            self.wishbone_slave.stb &
                                            ~self.wishbone_slave.ack &
                                            ~self.wishbone_slave.err &
                                            ~self.wishbone_slave.we &
                                            (wishbone_slave_timeout == 0),
                                            #led3.eq(1),
                                            If((self.master_read_buffer_addr == self.wishbone_slave.adr[2:30]) &
                                               (self.master_read_buffer_done[self.wishbone_slave.adr[0:2]]) &
                                               (~self.master_read_buffer_read[self.wishbone_slave.adr[0:2]]),
                                               ## use cache
                                               NextValue(self.wishbone_slave.ack, 1),
                                               NextValue(self.wishbone_slave.dat_r, Cat(self.master_read_buffer_data[self.wishbone_slave.adr[0:2]][24:32], # LE
                                                                                        self.master_read_buffer_data[self.wishbone_slave.adr[0:2]][16:24],
                                                                                        self.master_read_buffer_data[self.wishbone_slave.adr[0:2]][ 8:16],
                                                                                        self.master_read_buffer_data[self.wishbone_slave.adr[0:2]][ 0: 8])),
#                                               NextValue(self.wishbone_slave.dat_r, self.master_read_buffer_data[self.wishbone_slave.adr[0:2]]),
                                               #NextValue(self.led_display.value, Cat(Signal(8, reset = LED_M_READ | LED_M_CACHE), Signal(2, reset = 0), self.wishbone_slave.adr)), 
                                               NextValue(self.master_read_buffer_read[self.wishbone_slave.adr[0:2]], 1),
                                               NextValue(wishbone_slave_timeout, wishbone_default_timeout)
                                            ).Elif(~self.master_read_buffer_start,
                                                   #led2.eq(1),
                                                   NextValue(self.master_read_buffer_addr, self.wishbone_slave.adr[2:30]),
                                                   NextValue(self.master_read_buffer_done[0], 0),
                                                   NextValue(self.master_read_buffer_done[1], 0),
                                                   NextValue(self.master_read_buffer_done[2], 0),
                                                   NextValue(self.master_read_buffer_done[3], 0),
                                                   NextValue(self.master_read_buffer_read[0], 0),
                                                   NextValue(self.master_read_buffer_read[1], 0),
                                                   NextValue(self.master_read_buffer_read[2], 0),
                                                   NextValue(self.master_read_buffer_read[3], 0),
                                                   NextValue(last_word_idx, self.wishbone_slave.adr[0:2]),
                                                   NextValue(self.master_read_buffer_start, 1),
                                                   NextState("WaitForData")
                                            ).Else(
                                                #led1.eq(self.master_read_buffer_start)
                                            )
                                         )
        )
        wishbone_slave_buffering_fsm.act("WaitForData",
                                         #led2.eq(1),
                                         If(self.master_read_buffer_done[last_word_idx],
                                            NextValue(self.wishbone_slave.ack, 1),
                                            NextValue(self.wishbone_slave.dat_r, Cat(self.master_read_buffer_data[last_word_idx][24:32], # LE
                                                                                     self.master_read_buffer_data[last_word_idx][16:24],
                                                                                     self.master_read_buffer_data[last_word_idx][ 8:16],
                                                                                     self.master_read_buffer_data[last_word_idx][ 0: 8])),
#                                            NextValue(self.wishbone_slave.dat_r, self.master_read_buffer_data[last_word_idx]),
                                            NextValue(self.master_read_buffer_read[last_word_idx], 1),
                                            NextValue(wishbone_slave_timeout, wishbone_default_timeout),
                                            NextState("Idle")
                                         ),
                                         If(self.wishbone_slave.err,
                                            NextState("Idle")
                                         )
        )
        
