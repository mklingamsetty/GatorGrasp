from model import LeNet5

import torch
from esp_ppq.api import espdl_quantize_torch
from esp_ppq.executor.torch import TorchExecutor

from torchvision import datasets, transforms
from torch.utils.data import DataLoader, random_split
import torch.nn.functional as F
from torch.utils.data import DataLoader

ESPDL_MODEL_PATH = "./weights/model_weights.espdl"
INPUT_SHAPE = [1, 1, 32, 32]
TARGET = "esp32s3"
QUANT_TYPE = "w8a16" # 8-bit int, 16-bit float
DEVICE = "cpu"

def collate_fn(batch):
    # When iterating over TensorDataset, it returns Tuple(x, y). For quantization, only x is needed, not label y.
    batch = batch[0].to(DEVICE)
    return batch

def main():

    model = LeNet5()
    model.load_state_dict(torch.load("./weights/model_weights.pt"))
    model.eval()

    data_path = '../data/'
    mnist_test = datasets.MNIST(
        data_path, train=False, download=True,
        transform=transforms.Compose([
            transforms.ToTensor(),
            transforms.Pad(2)
        ]),
        target_transform=lambda y: F.one_hot(torch.tensor(y), num_classes=10).float()
    )
    dataloader = DataLoader(mnist_test, batch_size=32, shuffle=False)

    quant_ppq_graph = espdl_quantize_torch(
        model=model,
        espdl_export_file=ESPDL_MODEL_PATH,
        calib_dataloader=dataloader,
        calib_steps=32,
        input_shape=INPUT_SHAPE,
        inputs=None,
        target=TARGET,
        quant_type=QUANT_TYPE,
        collate_fn=collate_fn,
        device=DEVICE,
        error_report=True,
        skip_report=False,
        export_test_values=True,
        verbose=1,
    )

    correct = 0
    total = 0

    with torch.no_grad():
        for imgs, labels in dataloader:
            outputs = model(imgs)
            _, predicted = torch.max(outputs, dim=1)
            _, actual = torch.max(labels, dim=1)
            total += labels.shape[0]
            correct += int((predicted == actual).sum())

        print(f'Accuracy Original: {correct / total}')

    executor = TorchExecutor(graph=quant_ppq_graph, device=DEVICE)

    correct = 0
    total = 0

    with torch.no_grad():
        for imgs, labels in dataloader:
            outputs = executor.forward([imgs.to(DEVICE)])
            outputs = outputs[0] if isinstance(outputs, (list, tuple)) else outputs
            _, predicted = torch.max(outputs, dim=1)
            _, actual = torch.max(labels, dim=1)
            total += labels.shape[0]
            correct += int((predicted == actual).sum())

        print(f'Accuracy Quantized: {correct / total}')


if __name__ == "__main__":
    main()