import numpy as np
from emgtools import Myocell8
import torch
import torch.nn as nn

# Архитектура модели
class CNNModel(nn.Module):
    def __init__(self, input_size, num_classes):
        super(CNNModel, self).__init__()
        self.conv1 = nn.Conv1d(1, 40, kernel_size=200, stride=1, padding='same')
        self.conv2 = nn.Conv1d(40, 25, kernel_size=10, stride=1, padding='same')
        self.pool1 = nn.MaxPool1d(4)
        self.conv3 = nn.Conv1d(25, 100, kernel_size=10, stride=1, padding='same')
        self.conv4 = nn.Conv1d(100, 50, kernel_size=10, stride=1, padding='same')
        self.pool2 = nn.MaxPool1d(4)
        self.dropout = nn.Dropout(0.5)
        self.conv5 = nn.Conv1d(50, 100, kernel_size=10, stride=1, padding='same')
        self.global_pool = nn.AdaptiveAvgPool1d(1)
        self.fc = nn.Linear(100, num_classes)
        
    def forward(self, x):
        x = torch.relu(self.conv1(x))
        x = torch.relu(self.conv2(x))
        x = self.pool1(x)
        x = torch.relu(self.conv3(x))
        x = torch.relu(self.conv4(x))
        x = self.pool2(x)
        x = self.dropout(x)
        x = torch.relu(self.conv5(x))
        x = self.global_pool(x)
        x = x.view(x.size(0), -1)  # Выравнивание для полносвязного слоя
        x = torch.sigmoid(self.fc(x))
        return x

input_size=1024
num_classes=3
classifier = CNNModel(input_size, num_classes)
classifier.load_state_dict(torch.load('model100.pth'))
classifier.eval()
sftmax = nn.Softmax(dim=1)

# Channel to monitor
address = '192.168.1.89'
board = Myocell8([0,1,])
if board.connect(address):
    while True:
        if board.receive_data()>0:
            sample = torch.from_numpy(board.channels[1].muap_buf).unsqueeze(0).unsqueeze(1)
            with torch.no_grad():
                output = sftmax(classifier(sample))
                gesture_index = np.argmax(output.numpy())
                #winsound.Beep(300, 100)
