using Blackvolt.App.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Storage.Pickers;
using Feature = Blackvolt.App.Interop.NativeEngine.Feature;
using State = Blackvolt.App.Interop.NativeEngine.RuntimeState;
using Origin = Blackvolt.App.Interop.NativeEngine.RuntimeOrigin;

namespace Blackvolt.App.Views;

internal sealed record RuntimeRow(int FeatureIndex, string Name, string Detail, string StateText, Brush StateBrush);

public sealed partial class RuntimesPage : Page
{
    private static readonly Brush Ready = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 198, 255, 61));
    private static readonly Brush Warn = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 196, 61));
    private static readonly Brush Off = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 110, 110, 110));

    public RuntimesPage()
    {
        InitializeComponent();
        Refresh();
    }

    private void Refresh()
    {
        var engine = App.Engine;
        var rows = new List<RuntimeRow>();
        for (int i = 0; i < NativeEngine.FeatureCount; i++)
        {
            var feature = (Feature)i;
            if (feature == Feature.DlssNeuralRendering) continue; // internal stage, not a user-facing runtime
            var name = Runtimes.DisplayName(feature);
            if (engine is null)
            {
                rows.Add(new RuntimeRow(i, name, Runtimes.DllName(feature), "Motor detenido", Off));
                continue;
            }
            var s = engine.GetRuntimeStatus(feature);
            var origin = s.Origin switch
            {
                Origin.Official => "Oficial NVIDIA",
                Origin.Community => "Build de la comunidad",
                _ => "Versión no verificada",
            };
            var detail = s.State == State.Missing ? $"Falta {Runtimes.DllName(feature)}" : $"{s.LabelText}  ·  {origin}";
            var (text, brush) = s.State switch
            {
                State.Ready => ("Listo", Ready),
                State.Found => ("Disponible", Ready),
                State.UnsupportedGpu => ("Tu GPU no lo soporta", Warn),
                State.DriverTooOld => ("Actualiza el driver", Warn),
                State.NgxUnavailable => ("Solo GPUs NVIDIA", Off),
                _ => ("No instalado", Off),
            };
            rows.Add(new RuntimeRow(i, name, detail, text, brush));
        }
        RuntimeList.ItemsSource = rows;
    }

    private async void OnImportFile(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int index }) return;
        var feature = (Feature)index;
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add(".dll");
        InitializePicker(picker);
        var file = await picker.PickSingleFileAsync();
        if (file is null) return;

        try
        {
            Runtimes.Import(file.Path, feature);
            App.RestartEngine();
            Show(InfoBarSeverity.Success, $"{Runtimes.DisplayName(feature)} importado.");
        }
        catch (EngineException)
        {
            Show(InfoBarSeverity.Error, $"Ese archivo no es {Runtimes.DllName(feature)} ni una versión conocida.");
        }
        Refresh();
    }

    private async void OnImportFromFolder(object sender, RoutedEventArgs e)
    {
        var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add("*");
        InitializePicker(picker);
        var folder = await picker.PickSingleFolderAsync();
        if (folder is null) return;

        var candidates = Runtimes.Scan(folder.Path);
        if (candidates.Count == 0)
        {
            Show(InfoBarSeverity.Warning, "No se encontraron DLLs de DLSS ni RTX Video en esa carpeta.");
            return;
        }

        // One file per feature: the scan lists known builds first.
        var imported = new List<string>();
        foreach (var c in candidates.GroupBy(c => c.Feature).Select(g => g.First()))
        {
            try
            {
                Runtimes.Import(c.Path, c.Feature);
                imported.Add(Runtimes.DisplayName(c.Feature));
            }
            catch (EngineException) { }
        }
        App.RestartEngine();
        Show(imported.Count > 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning,
            imported.Count > 0 ? "Importado: " + string.Join(", ", imported) + "." : "No se pudo importar ninguna DLL.");
        Refresh();
    }

    private void Show(InfoBarSeverity severity, string message)
    {
        Notice.Severity = severity;
        Notice.Message = message;
        Notice.IsOpen = true;
    }

    private static void InitializePicker(object picker)
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
    }
}
