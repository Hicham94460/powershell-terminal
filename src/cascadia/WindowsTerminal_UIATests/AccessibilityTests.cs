//----------------------------------------------------------------------------------------------------------------------
// <copyright file="SmokeTests.cs" company="Microsoft">
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// </copyright>
// <summary>UI Automation tests for Axe.Windows to perform an accessibility pass on different scenarios.</summary>
//----------------------------------------------------------------------------------------------------------------------

namespace WindowsTerminal.UIA.Tests
{
    using Axe.Windows.Automation;
    using OpenQA.Selenium;
    using System.Linq;
    using System;
    using WEX.TestExecution.Markup;
    using WEX.Logging.Interop;

    using WindowsTerminal.UIA.Tests.Common;
    using WindowsTerminal.UIA.Tests.Elements;

    [TestClass]
    public class AccessibilityTests
    {
        public TestContext TestContext { get; set; }

        private OpenQA.Selenium.Appium.AppiumWebElement OpenSUIPage(TerminalApp app, string pageAutomationId)
        {
            var root = app.GetRoot();

            root.SendKeys(Keys.LeftControl + ",");
            Globals.WaitForTimeout();

            // Navigate to the settings page
            root.FindElementByAccessibilityId(pageAutomationId);
            Globals.WaitForTimeout();

            return root;
        }

        private IScanner BuildScanner(int processId)
        {
            var builder = Config.Builder.ForProcessId(processId);
            builder.WithOutputFileFormat(OutputFileFormat.A11yTest);
            builder.WithOutputDirectory(TestContext.TestRunDirectory); // TODO CARLOS: lol, no idea where this goes
            var config = builder.Build();
            return ScannerFactory.CreateScanner(config);
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIStartup()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                // TODO CARLOS: I expanded OpenSUIPage() here for debugging purposes
                //OpenSUIPage(app, "LaunchNavItem");
                var root = app.GetRoot();

                root.SendKeys(Keys.LeftControl + ",");
                Globals.WaitForTimeout();

                // Navigate to the settings page
                root.FindElementByAccessibilityId("LaunchNavItem");
                Globals.WaitForTimeout();

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIInteraction()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                OpenSUIPage(app, "InteractionNavItem");

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIAppearance()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                OpenSUIPage(app, "AppearanceNavItem");

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIColorSchemes()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                var root = OpenSUIPage(app, "ColorSchemesNavItem");

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                // TODO CARLOS: actually navigate
                // Navigate to the Campbell scheme and scan it
                //root.FindElementByAccessibilityId("");
                Globals.WaitForTimeout();
                scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIRendering()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                OpenSUIPage(app, "RenderingNavItem");

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIActions()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                OpenSUIPage(app, "ActionsNavItem");

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIBaseLayer()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                var root = OpenSUIPage(app, "BaseLayerNavItem");

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                // TODO CARLOS: actually navigate
                // Navigate to the Appearance page and scan it
                //root.FindElementByAccessibilityId("");
                Globals.WaitForTimeout();
                scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                // TODO CARLOS: actually navigate
                // Navigate to the Advanced page and scan it
                //root.FindElementByAccessibilityId("");
                Globals.WaitForTimeout();
                scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }

        [TestMethod]
        [TestProperty("A11yInsights", "true")]
        public void RunSUIAddProfile()
        {
            using (TerminalApp app = new TerminalApp(TestContext))
            {
                OpenSUIPage(app, "AddProfileNavItem");

                // Run Axe.Windows on scenario
                var scanner = BuildScanner(app.ProcessId);
                var scanOutput = scanner.Scan(null);
                Log.Comment($"Errors Found: {scanOutput.WindowScanOutputs.First().ErrorCount}");

                Globals.WaitForLongTimeout();
            }
        }
    }
}
