/* FloodMesh website settings. Edit these, nothing else, to go live. */
window.FM_CONFIG = {
  // true: shows the "draft" note on the privacy page. Set false at launch.
  // Whether orders are sent depends only on orderEndpoint below.
  preview: true,

  // "/api/preorder" once the Vercel function and the Google Sheet script are
  // set up (apps-script/README.md). Empty: orders are not sent anywhere, and
  // the form only shows what it would send.
  orderEndpoint: "",

  // Shown on the site when filled in. Leave empty to hide.
  contactEmail: "support@dverselabs.com",
  contactPhone: "+91 93602 47270",
};
