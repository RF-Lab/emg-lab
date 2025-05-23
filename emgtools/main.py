import numpy as np
import plotly.graph_objects as go
from emgtools import Myocell8
from tqdm import tqdm
import matplotlib.pyplot as plt
import matplotlib.animation as animation

board = Myocell8([1,2,3,4])
fig, ax = plt.subplots()
line = ax.plot(board.channels[0].cyclic_buf)[0]
ax.grid(True)

def update(frame):
    board.receive_data()
    line.set_xdata(np.arange(len(board.channels[1].cyclic_buf)))
    data = board.channels[1].cyclic_buf
    line.set_ydata(data)
    ax.set_ylim(data.min(), data.max())
    return line

if board.connect('192.168.1.57'):
    ani = animation.FuncAnimation(fig=fig, func=update, interval=1)
    plt.show()
    # pbar = tqdm(range(200))
    # for _ in pbar:
    #     board.receive_data()
    #     pbar.set_description(f'#{board.block_count}')

# fig = go.Figure()
# fig.add_scatter(y=board.channels[2].cyclic_buf)
# fig.write_html('tmp.html')

