import socket
from struct import *
import numpy as np

class EmgChannel:
    cyclic_buf = np.zeros((2048,),dtype=np.float64)
    def on_data_receive(self,data):
        self.cyclic_buf = np.roll(self.cyclic_buf,-len(data))
        self.cyclic_buf[-len(data):] = data
        return False

class EmgSource:
    num_channels_triggered = None
    channels = []
    def __init__(self,channels_to_receive):
        for ch in channels_to_receive:
            self.channels.append( EmgChannel() )
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
    SAMPLES_PER_TRANSPORT_BLOCK = 64
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

        


