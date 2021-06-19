# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Tests for pw_console.console_app"""

import platform
import unittest

from prompt_toolkit.application import create_app_session
# inclusive-language: ignore
from prompt_toolkit.output import DummyOutput as FakeOutput

from pw_console.console_app import ConsoleApp


class TestConsoleApp(unittest.TestCase):
    """Tests for ConsoleApp."""
    def test_instantiate(self) -> None:
        # TODO(tonymd): Find out why create_app_session isn't working here on
        # windows.
        if platform.system() in ['Windows']:
            return
        with create_app_session(output=FakeOutput()):
            console_app = ConsoleApp()
            self.assertIsNotNone(console_app)


if __name__ == '__main__':
    unittest.main()