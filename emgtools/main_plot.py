import numpy as np
from emgtools import Myocell8
import matplotlib.pyplot as plt
import matplotlib.animation as animation

# Channel to monitor
channels = [1,]
address = '192.168.1.89'

board = Myocell8([0,1,2,3,4])
fig, ax = plt.subplots()
lines = []
for n,channel in enumerate(channels):
    lines.append(ax.plot(board.channels[channel].cyclic_buf,label=f'channel {channel}')[0])
    lines.append(ax.plot(board.channels[channel].filt_cyclic_buf,label=f'filtered channel {channel}')[0])
    lines.append(ax.plot(board.channels[channel].env_cyclic_buf,label=f'envelop from channel {channel}')[0])
ax.set_facecolor((0.8,0.9,0.9))
ax.grid(True)
#ax.legend()

def update(frame):
    board.receive_data()
    max_v = -1E10
    min_v = 1E10
    for n,channel in enumerate(channels):
        # lines[n].set_xdata(np.arange(len(board.channels[channel].cyclic_buf)))
        # data = board.channels[channel].cyclic_buf
        # lines[n].set_ydata(data)
        # max_v = max([max_v, data.max()])
        # min_v = min([min_v, data.min()])

        lines[n+1].set_xdata(np.arange(len(board.channels[channel].filt_cyclic_buf)))
        data = board.channels[channel].filt_cyclic_buf
        lines[n+1].set_ydata(data)
        max_v = max([max_v, data.max()])
        min_v = min([min_v, data.min()])

        lines[n+2].set_xdata(np.arange(len(board.channels[channel].env_cyclic_buf)))
        data = board.channels[channel].env_cyclic_buf
        lines[n+2].set_ydata(data)
        max_v = max([max_v, data.max()])
        min_v = min([min_v, data.min()])

    #ax.set_ylim(min_v-np.abs(min_v)*0.1, max_v+np.abs(max_v)*0.1)
    ax.set_ylim([-0.1,0.1])
    ax.set_title(f'Block #{board.block_count}')
    return lines

if board.connect(address):
    ani = animation.FuncAnimation(fig=fig, func=update, interval=1)
    plt.show()

