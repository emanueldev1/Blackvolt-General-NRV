using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace Blackvolt.App.Views;

public sealed partial class ComingSoonPage : Page
{
    public ComingSoonPage() => InitializeComponent();

    protected override void OnNavigatedTo(NavigationEventArgs e) =>
        Heading.Text = e.Parameter as string is { Length: > 0 } title ? title : "Próximamente";
}
