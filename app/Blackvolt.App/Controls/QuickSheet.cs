using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;

namespace Blackvolt.App.Controls;

/// <summary>
/// Floating side sheet over the video (Veyra's quick panel): every enhancement and session option
/// in tabs, without leaving playback or the console connection. Opens with the Home key or the
/// toolbar button; Esc or the close button hides it.
/// </summary>
internal sealed class QuickSheet : UserControl
{
    private readonly Border _panel = new();
    private readonly TranslateTransform _shift = new();
    private readonly TextBlock _subtitle;
    private bool _open;

    public event Action<bool>? OpenChanged;

    public QuickSheet(string title, UIElement body)
    {
        _panel.Style = (Style)Application.Current.Resources["BvPanelStyle"];
        _panel.Padding = new Thickness(18, 16, 4, 12);
        Width = 424;
        Margin = new Thickness(12, 76, 12, 12);
        HorizontalAlignment = HorizontalAlignment.Right;
        VerticalAlignment = VerticalAlignment.Stretch;
        Visibility = Visibility.Collapsed;
        RenderTransform = _shift;
        _panel.Shadow = new ThemeShadow();
        _panel.Translation = new System.Numerics.Vector3(0, 0, 32);

        var grid = new Grid { RowSpacing = 12 };
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        var head = new Grid { Padding = new Thickness(0, 0, 14, 0) };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var titles = new StackPanel { Spacing = 1 };
        titles.Children.Add(new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeights.SemiBold });
        _subtitle = new TextBlock
        {
            FontSize = 12,
            Foreground = (Brush)Application.Current.Resources["BvMutedBrush"],
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        titles.Children.Add(_subtitle);
        head.Children.Add(titles);
        var close = new Button
        {
            Style = (Style)Application.Current.Resources["BvIconButtonStyle"],
            Content = new FontIcon { Glyph = "", FontSize = 12 },
            VerticalAlignment = VerticalAlignment.Top,
        };
        ToolTipService.SetToolTip(close, "Cerrar (Esc o Inicio)");
        close.Click += (_, _) => IsOpen = false;
        Grid.SetColumn(close, 1);
        head.Children.Add(close);
        grid.Children.Add(head);

        Grid.SetRow((FrameworkElement)body, 1);
        grid.Children.Add(body);
        _panel.Child = grid;
        Content = _panel;
    }

    /// <summary>Second line of the header: what is running right now (e.g. "DLSS NR · 4.1 ms").</summary>
    public string Subtitle
    {
        get => _subtitle.Text;
        set => _subtitle.Text = value;
    }

    public bool IsOpen
    {
        get => _open;
        set
        {
            if (_open == value) return;
            _open = value;
            Animate(value);
            OpenChanged?.Invoke(value);
        }
    }

    public void Toggle() => IsOpen = !IsOpen;

    private void Animate(bool show)
    {
        var reduce = Services.AppSettings.Current.Ui.ReduceMotion;
        if (show) Visibility = Visibility.Visible;
        if (reduce)
        {
            Opacity = show ? 1 : 0;
            _shift.X = 0;
            if (!show) Visibility = Visibility.Collapsed;
            return;
        }
        var duration = new Duration(TimeSpan.FromMilliseconds(show ? 190 : 140));
        var ease = new CubicEase { EasingMode = show ? EasingMode.EaseOut : EasingMode.EaseIn };
        var slide = new DoubleAnimation { From = show ? 36 : 0, To = show ? 0 : 36, Duration = duration, EasingFunction = ease };
        var fade = new DoubleAnimation { From = show ? 0 : 1, To = show ? 1 : 0, Duration = duration, EasingFunction = ease };
        Storyboard.SetTarget(slide, _shift);
        Storyboard.SetTargetProperty(slide, "X");
        Storyboard.SetTarget(fade, this);
        Storyboard.SetTargetProperty(fade, "Opacity");
        var board = new Storyboard();
        board.Children.Add(slide);
        board.Children.Add(fade);
        if (!show) board.Completed += (_, _) => { if (!_open) Visibility = Visibility.Collapsed; };
        board.Begin();
    }

    /// <summary>True when the element is inside this sheet (keys typed there must not reach the console).</summary>
    public bool Contains(DependencyObject? element)
    {
        for (var e = element; e is not null; e = VisualTreeHelper.GetParent(e))
            if (ReferenceEquals(e, this)) return true;
        return false;
    }
}
