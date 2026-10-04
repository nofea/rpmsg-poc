if request.isInit:
    tx_val = 0
    rx_val = 0

if request.isRead:
    if request.offset == 0x0: # TX Reg
        request.value = tx_val
    elif request.offset == 0x4: # RX Reg
        request.value = rx_val
        if hasattr(self, 'IRQ'):
            self.IRQ.Set(False)
elif request.isWrite:
    if request.offset == 0x0:
        tx_val = request.value
        # Trigger R5F interrupt
        if hasattr(self, 'IRQ'):
            self.IRQ.Set(True)
    elif request.offset == 0x4:
        rx_val = request.value
        if hasattr(self, 'IRQ'):
            self.IRQ.Set(False)
