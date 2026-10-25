from matplotlib import pyplot as plt
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim

from torchvision import datasets, transforms
from torch.utils.data import DataLoader, random_split
import torch.nn.functional as F

from model import LeNet5

def train(epochs, optimizer, model, loss_fn, train_loader, val_loader):
    for epoch in range(1, epochs + 1):
        loss_train = 0.0
        for imgs, labels in train_loader:
            outputs = model(imgs)
            loss = loss_fn(outputs, labels)

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            loss_train += loss.item()

        with torch.no_grad():
            val_loss = 0.0
            for imgs, labels in val_loader:
                outputs = model(imgs)
                loss = loss_fn(outputs, labels)

                val_loss += loss.item()

        
        print(f'Epoch {epoch}, Training loss {loss_train / len(train_loader)}, Val loss: {val_loss / len(val_loader)}')

def evaluate(model, train_loader, val_loader, test_loader):
    for name, loader in [('train', train_loader), ('val', val_loader), ('test', test_loader)]:
        correct = 0
        total = 0

        with torch.no_grad():
            for imgs, labels in loader:
                outputs = model(imgs)
                _, predicted = torch.max(outputs, dim=1)
                _, actual = torch.max(labels, dim=1)
                total += labels.shape[0]
                correct += int((predicted == actual).sum())

        print(f'Accuracy {name}: {correct / total}')



data_path = '../data/'
mnist = datasets.MNIST(
    data_path, train=True, download=True,
    transform=transforms.Compose([
        transforms.ToTensor(),
        transforms.Pad(2)
    ]),
    target_transform=lambda y: F.one_hot(torch.tensor(y), num_classes=10).float()
)

mnist_train, mnist_val = random_split(
    mnist,
    [0.7, 0.3],
    generator=torch.Generator().manual_seed(42)
)

mnist_test = datasets.MNIST(
    data_path, train=False, download=True,
    transform=transforms.Compose([
        transforms.ToTensor(),
        transforms.Pad(2)
    ]),
    target_transform=lambda y: F.one_hot(torch.tensor(y), num_classes=10).float()
)

train_loader = DataLoader(mnist_train, batch_size=64, shuffle=True)
val_loader = DataLoader(mnist_val, batch_size=64, shuffle=False)
test_loader = DataLoader(mnist_test, batch_size=64, shuffle=False)

model = LeNet5()
optimizer = optim.Adam(model.parameters())
loss_fn = nn.CrossEntropyLoss()

train(
    epochs = 50,
    optimizer = optimizer,
    model = model,
    loss_fn = loss_fn,
    train_loader = train_loader,
    val_loader = val_loader
)

evaluate(model, train_loader, val_loader, test_loader)

torch.save(model.state_dict(), './weights/model_weights.pt')