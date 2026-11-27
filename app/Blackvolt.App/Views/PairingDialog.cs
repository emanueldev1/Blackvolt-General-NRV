using Blackvolt.App.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Kind = Blackvolt.App.Interop.NativeRemote.ConsoleKind;
using PairState = Blackvolt.App.Interop.NativeRemote.PairingState;

namespace Blackvolt.App.Views;

/// <summary>Pairs a console with the 8-digit code from its Remote Play settings.</summary>
internal sealed class PairingDialog : ContentDialog
{
    private readonly ConsoleLibrary _library;
    private readonly TextBox _host;
    private readonly ComboBox _kind;
    private readonly TextBox _account;
    private readonly TextBox _pin;
    private readonly ProgressRing _ring = new() { IsActive = false, Width = 20, Height = 20 };
    private readonly TextBlock _status = new() { TextWrapping = TextWrapping.Wrap };

    public bool Paired { get; private set; }

    public PairingDialog(ConsoleLibrary library, string? host, Kind kind, bool hasStoredAccount)
    {
        _library = library;
        Title = "Emparejar consola";
        PrimaryButtonText = "Emparejar";
        CloseButtonText = "Cancelar";
        DefaultButton = ContentDialogButton.Primary;

        _host = new TextBox { Header = "Dirección de la consola", Text = host ?? "", PlaceholderText = "192.168.1.50" };
        _kind = new ComboBox { Header = "Consola", HorizontalAlignment = HorizontalAlignment.Stretch, Items = { "PS5", "PS4" } };
        _kind.SelectedIndex = kind == Kind.PS4 ? 1 : 0;
        _account = new TextBox
        {
            Header = "ID de cuenta de PSN",
            PlaceholderText = hasStoredAccount ? "Se usa el de tus consolas importadas" : "Base64 (p. ej. AQIDBAUGBwg=) o número",
        };
        _pin = new TextBox { Header = "Código de 8 dígitos", MaxLength = 8, PlaceholderText = "12345678", InputScope = Numeric() };

        var help = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Opacity = 0.75,
            Text = "En la consola: Ajustes › Sistema › Uso a distancia › Vincular dispositivo (PS5) o " +
                   "Ajustes › Conexión de Uso a distancia › Añadir dispositivo (PS4). Deja la pantalla con el código abierta.",
        };
        Content = new StackPanel
        {
            Spacing = 12,
            MinWidth = 420,
            Children =
            {
                help, _host, _kind, _account, _pin,
                new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Children = { _ring, _status } },
            },
        };
        PrimaryButtonClick += OnPrimary;
    }

    private static Microsoft.UI.Xaml.Input.InputScope Numeric()
    {
        var scope = new Microsoft.UI.Xaml.Input.InputScope();
        scope.Names.Add(new Microsoft.UI.Xaml.Input.InputScopeName(Microsoft.UI.Xaml.Input.InputScopeNameValue.Number));
        return scope;
    }

    private async void OnPrimary(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        var deferral = args.GetDeferral();
        try
        {
            var host = _host.Text.Trim();
            if (host.Length == 0) { Fail("Escribe la dirección IP de la consola."); args.Cancel = true; return; }
            if (_pin.Text.Length != 8 || !uint.TryParse(_pin.Text, out var pin)) { Fail("El código debe tener 8 dígitos."); args.Cancel = true; return; }

            IsPrimaryButtonEnabled = false;
            _ring.IsActive = true;
            _status.Text = "Emparejando…";
            ConsolePairing pairing;
            try
            {
                pairing = ConsolePairing.Start(_library, host, _kind.SelectedIndex == 1 ? Kind.PS4 : Kind.PS5, _account.Text, pin);
            }
            catch (EngineException)
            {
                Fail(_account.Text.Trim().Length == 0
                    ? "Falta el ID de cuenta de PSN (impórtalo desde chiaki-ng/Veyra o escríbelo)."
                    : "El ID de cuenta no es válido.");
                args.Cancel = true;
                return;
            }
            using (pairing)
            {
                while (true)
                {
                    await Task.Delay(250);
                    var (state, error) = pairing.Poll();
                    if (state == PairState.Running) continue;
                    if (state == PairState.Succeeded)
                    {
                        Paired = true;
                        return; // dialog closes with Primary
                    }
                    Fail(state == PairState.Canceled ? "Cancelado." : (error.Length > 0 ? error : "La consola rechazó el emparejamiento."));
                    args.Cancel = true;
                    return;
                }
            }
        }
        finally
        {
            _ring.IsActive = false;
            IsPrimaryButtonEnabled = true;
            deferral.Complete();
        }
    }

    private void Fail(string message) => _status.Text = message;
}
