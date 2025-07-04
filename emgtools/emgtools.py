import socket
from struct import *
import numpy as np
from scipy import signal


class EmgChannel:
    cyclic_buf_size = 2048
    muap_size = 1024
    spike_width = 50
    spike_th = 0.05
    mvc_max = 50000.0 # Maximum voluntary contraction 
    state = 0 # no spike detected
    pause_sample_count = 0
    def __init__(self,fs):
        self.fs = fs
        # Полосовой КИХ фильтр с подавлением 50 Гц,
        # Метод расчета - Оконное взвешивание
        if self.fs==1000:
            self.fltFirWnd = signal.firwin( 256, [10,45, 55,  450], pass_zero=False, fs=self.fs )
            self.fltFirEnv = signal.firwin( 128, [20,], pass_zero=True, fs=self.fs )
        else:
            print(f'{self.__class__}: Error: unsupported sampling frequency {fs}')
        #f, h = signal.freqz(fltFirWnd,fs=1000)
        # fig = go.Figure()
        # fig.add_scatter(x=f,y=10*np.log10(np.square(np.abs(h))),name='Полосовой КИХ фильтр с подавлением 50 Гц',mode='lines')
        # update_freqz_layout(fig)
        # fig.update_layout(title='АЧХ предварительного фильтра')
        # fig.show()
        # np.savetxt( 'filter.txt', fltFirWnd )
        self.cyclic_buf = np.zeros((self.cyclic_buf_size,),dtype=np.float64)
        self.filt_cyclic_buf = np.zeros((self.cyclic_buf_size,),dtype=np.float64)
        self.env_cyclic_buf = np.zeros((self.cyclic_buf_size,),dtype=np.float64)
        self.muap_buf = np.zeros((self.muap_size,),dtype=np.float64)
    def on_data_receive(self,data):
        # Update cyclic buffer
        self.cyclic_buf = np.roll(self.cyclic_buf,-len(data))
        self.cyclic_buf[-len(data):] = data
        # Update filtered cyclic buffer
        self.filt_cyclic_buf = np.roll(self.filt_cyclic_buf,-len(data))
        filt_data = np.convolve( self.fltFirWnd, self.cyclic_buf[-(len(data)+len(self.fltFirWnd)):], 'same' )
        self.filt_cyclic_buf[-len(data):] = filt_data[-(len(data)+len(self.fltFirWnd)//2):-len(self.fltFirWnd)//2]/self.mvc_max
        # Update envelope cyclic buffer
        self.env_cyclic_buf = np.roll(self.env_cyclic_buf,-len(data))
        filt_data = np.convolve( self.fltFirEnv, np.abs(self.filt_cyclic_buf[-(len(data)+len(self.fltFirEnv)):]), 'same' )
        self.env_cyclic_buf[-len(data):] = filt_data[-(len(data)+len(self.fltFirEnv)//2):-len(self.fltFirEnv)//2]
        if self.pause_sample_count>0:
            self.pause_sample_count -= len(data)
            if self.pause_sample_count<0:
                self.pause_sample_count = 0
        else:
            # Muap detection
            det_area = self.env_cyclic_buf[-self.muap_size:]
            if np.count_nonzero(det_area[self.muap_size//2-self.spike_width//2:self.muap_size//2+self.spike_width//2]>self.spike_th)>self.spike_width//2: #np.all(det_area[self.muap_size//2-self.spike_width//2:self.muap_size//2+self.spike_width//2]>self.spike_th):
                if np.all(det_area[:self.spike_width]<self.spike_th):
                    if np.all(det_area[-self.spike_width:]<self.spike_th):
                        self.muap_buf = self.filt_cyclic_buf[-self.muap_size:]
                        self.pause_sample_count = 1024
                        return True
        return False

class EmgSource:
    num_channels_triggered = None
    channels = []
    def __init__(self,channels_to_receive):
        for ch in channels_to_receive:
            self.channels.append( EmgChannel(fs=1000) )
    def connect(self,ip_address):
        print('Error: connect method should be overriden')
        return -1
    def read_from_source(self):
        print('Error: read_from_source method should be overriden')
        return -1
    def on_channel_data_recv(self,chIdx,channelData):
        # print(f'channel {chIdx} updated')
        return self.channels[chIdx].on_data_receive(channelData)
        
class Myocell8(EmgSource):
    TCP_BOARD_SERVER_PORT = 3000
    TRANSPORT_BLOCK_HEADER_SIZE = 16
    PKT_COUNT_OFFSET = 2
    SAMPLES_PER_TRANSPORT_BLOCK = 128
    NUM_CHANNELS = 8
    tcp_packet_size = None
    receivedBuffer = bytes()
    sock = None
    block_count = None
    def __init__(self, channels_to_receive):
        super().__init__(channels_to_receive)
        self.tcp_packet_size = int(((self.TRANSPORT_BLOCK_HEADER_SIZE)/4+(self.NUM_CHANNELS+1)*(self.SAMPLES_PER_TRANSPORT_BLOCK))*4)
        self.channels_to_receive = channels_to_receive
        self.block_count = 0
    def connect(self,ip_address):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # Connect the socket to the port where the server is listening
        server_address = (ip_address, self.TCP_BOARD_SERVER_PORT)
        try:
            self.sock.connect(server_address)
        except socket.error as msg:
            print(f'{self.__class__}: sock.connect failed:{msg}')
            return False
        print(f'{self.__class__}: Connected to {server_address}')
        return True
    def save_last_muap(self,ges_type,ges_num):
        np.savetxt( f'./gestures/{ges_type:1d}/p_{ges_type:1d}_{ges_num}.txt', self.channels[1].muap_buf )
    def receive_data(self):
        self.num_channels_triggered = 0
        if len(self.receivedBuffer)>=self.tcp_packet_size*2:
            # find sync bytes
            startOfBlock = self.receivedBuffer.find('EMG8x'.encode())
            if startOfBlock>=0:
                # print('>on EMG8x')
                # SAMPLES_PER_TRANSPORT_BLOCK*(AD1299_NUM_CH+1)+TRANSPORT_BLOCK_HEADER_SIZE/4
                block_count = unpack('1i',self.receivedBuffer[startOfBlock+8:startOfBlock+12])[0]
                if self.block_count!=block_count:
                    print(f'{self.__class__}: resync @block count:{block_count}')
                    self.block_count = block_count
                strFormat = '{:d}i'.format(round(self.SAMPLES_PER_TRANSPORT_BLOCK*(self.NUM_CHANNELS+1)+self.TRANSPORT_BLOCK_HEADER_SIZE/4))
                #'1156i'
                samples = unpack(strFormat, self.receivedBuffer[startOfBlock:startOfBlock+self.tcp_packet_size] )
        
                # remove block from received buffer
                self.receivedBuffer = self.receivedBuffer[startOfBlock+self.tcp_packet_size:]
        
                for chIdx,physChNum in enumerate(self.channels_to_receive):
                            
                    # get channel offset
                    offset_toch    =  int(self.TRANSPORT_BLOCK_HEADER_SIZE/4 + self.SAMPLES_PER_TRANSPORT_BLOCK*physChNum) 
        
                    #print( samples[offset_to4ch:offset_to4ch+SAMPLES_PER_TRANSPORT_BLOCK] )
                    dataSamples = samples[offset_toch:offset_toch+self.SAMPLES_PER_TRANSPORT_BLOCK]
        
                    blockSamples = np.array(dataSamples)
                    if self.on_channel_data_recv(chIdx, blockSamples):
                        self.num_channels_triggered += 1
                self.block_count += 1
            else:
                # remove block from received buffer
                print('{self.__class__}: clean up receivedBuffer')
                self.receivedBuffer = bytes()
        else:
            try:
                receivedData = self.sock.recv( self.tcp_packet_size )
            except socket.error as msg:
                print(f'{self.__class__}: sock.recv failed:{msg}')
                return -1
            if not receivedData:
                # probably connection closed
                return -1    
            self.receivedBuffer += receivedData
        return self.num_channels_triggered

        


