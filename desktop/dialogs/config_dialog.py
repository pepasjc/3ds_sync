from PyQt6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QLineEdit,
    QMessageBox,
)

from config import load_config, save_config


class ConfigDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Server Configuration")
        self.setMinimumSize(400, 200)
        self._init_ui()
        self._load_config()

    def _init_ui(self):
        layout = QFormLayout(self)

        self.host_edit = QLineEdit()
        self.host_edit.setPlaceholderText("localhost")
        layout.addRow("Server Host:", self.host_edit)

        self.port_edit = QLineEdit()
        self.port_edit.setPlaceholderText("8000")
        layout.addRow("Server Port:", self.port_edit)

        self.api_key_edit = QLineEdit()
        self.api_key_edit.setPlaceholderText("anything")
        layout.addRow("API Key:", self.api_key_edit)

        self.sd_card_edit = QLineEdit()
        self.sd_card_edit.setPlaceholderText("e.g. K:  — drive where the SD reader is mounted now")
        self.sd_card_edit.setToolTip(
            "Current drive of your removable SD card reader.\n"
            "Profiles marked \"This is an SD card\" have their drive letter\n"
            "rewritten to this value, so a changing drive letter no longer breaks them."
        )
        layout.addRow("Current SD Card Drive:", self.sd_card_edit)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Save
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._save)
        buttons.rejected.connect(self.reject)
        layout.addRow(buttons)

    def _load_config(self):
        config = load_config()
        self.host_edit.setText(config.get("host", "localhost"))
        self.port_edit.setText(str(config.get("port", "8000")))
        self.api_key_edit.setText(config.get("api_key", "anything"))
        self.sd_card_edit.setText(config.get("sd_card_location", ""))

    def _save(self):
        try:
            port = int(self.port_edit.text())
        except ValueError:
            QMessageBox.warning(self, "Invalid Port", "Port must be a number")
            return

        config = load_config()
        config["host"] = self.host_edit.text() or "localhost"
        config["port"] = port
        config["api_key"] = self.api_key_edit.text() or "anything"
        config["sd_card_location"] = self.sd_card_edit.text().strip()
        save_config(config)
        self.accept()
