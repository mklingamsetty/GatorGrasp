import torch
import torch.nn as nn

class LeNet5(nn.Module):
    def __init__(self):
        super().__init__()

        self.classifier = nn.Sequential(
            nn.Conv2d(in_channels=1, out_channels=6, kernel_size=5), # 1 input channel, 6 output channels (feature maps), 5x5 kernel, no padding -> 32x32 to 28x28
            nn.ReLU(),
            nn.AvgPool2d(kernel_size=2, stride=2), # unsure if this is the correct pooling type
            
            nn.Conv2d(in_channels=6, out_channels=16, kernel_size=5), # 6 input channels, 16 output channels
            nn.ReLU(),
            nn.AvgPool2d(kernel_size=2, stride=2),

            nn.Conv2d(in_channels=16, out_channels=120, kernel_size=5),
            nn.ReLU(),
            nn.Flatten(),

            nn.Linear(120,84),
            nn.ReLU(),

            nn.Linear(84,10)
        )
        
    def forward(self, x):
        return self.classifier(x)