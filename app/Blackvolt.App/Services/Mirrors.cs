namespace Blackvolt.App.Services;

/// <summary>
/// Download routes for GitHub assets, the same three Veyra offers: GitHub directly, or through the
/// gh-proxy / ghfast mirrors for networks where GitHub's CDN is slow or blocked.
/// </summary>
internal static class Mirrors
{
    public static readonly string[] Names = ["GitHub (directo)", "gh-proxy.com", "ghfast.top"];

    /// <summary>Route for a release asset (github.com/.../releases/download/...).</summary>
    public static string Asset(string url, int mirror) => mirror switch
    {
        1 => "https://gh-proxy.com/" + url,
        2 => "https://ghfast.top/" + url,
        _ => url,
    };

    /// <summary>Route for the GitHub REST API. ghfast only serves release assets.</summary>
    public static string Api(string url, int mirror) => mirror == 1 ? "https://gh-proxy.com/" + url : url;
}
